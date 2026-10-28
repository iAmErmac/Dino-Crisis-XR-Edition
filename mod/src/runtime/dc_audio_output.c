#include "dc_audio_output.h"

#include <math.h>
#include <string.h>

enum {
    DC_AUDIO_MAX_INPUT_FRAMES = 2048,
    DC_AUDIO_GAP_FRAMES = 64
};

static uint32_t required_capacity(uint32_t frames, int drc)
{
    if (!drc) return frames;
    return frames + (frames + 19u) / 20u + 2u;
}

static int valid_state(const DcAudioOutput *state)
{
    return state && isfinite(state->position) && state->position >= -1.0 &&
        state->position < 1.0 && isfinite(state->average_queue) &&
        state->average_queue >= -1.0 &&
        isfinite(state->rate_adjust) && state->rate_adjust >= -0.05 &&
        state->rate_adjust <= 0.05 &&
        state->fade_remaining <= state->fade_total &&
        (state->gap == 0 || state->gap == 1);
}

static int valid_config(const DcAudioOutputConfig *config)
{
    return config && isfinite(config->max_adjust) &&
        (!config->drc || config->target_frames != 0u);
}

static double effective_adjust(double max_adjust)
{
    if (max_adjust <= 0.0) return 0.0;
    return max_adjust < 0.05 ? max_adjust : 0.05;
}

static int16_t clamp_sample(long value)
{
    if (value > 32767L) return 32767;
    if (value < -32768L) return -32768;
    return (int16_t)value;
}

static int16_t interpolate_sample(double value)
{
    if (value >= 32767.0) return 32767;
    if (value <= -32768.0) return -32768;
    return clamp_sample(lround(value));
}

static int count_resampled(double position, uint32_t frames, double step,
    uint32_t *count, double *end_position)
{
    const double limit = (double)(frames - 1u);
    uint32_t output_frames = 0;

    while (position < limit) {
        if (output_frames >= required_capacity(frames, 1)) return 0;
        ++output_frames;
        position += step;
    }

    *count = output_frames;
    *end_position = position - (double)frames;
    return 1;
}

static int32_t bridge_sample(int16_t value, int32_t delta, uint32_t index,
    uint32_t count)
{
    int32_t bridged = (int32_t)value + delta * (int32_t)(count - index) /
        (int32_t)count;
    if (bridged > 32767) bridged = 32767;
    if (bridged < -32768) bridged = -32768;
    return bridged;
}

static void apply_gap(DcAudioOutput *state, int16_t *output,
    uint32_t frames)
{
    const uint32_t count = frames < DC_AUDIO_GAP_FRAMES ? frames :
        DC_AUDIO_GAP_FRAMES;

    for (int channel = 0; channel < 2; ++channel) {
        const int32_t delta = (int32_t)state->last_output[channel] - output[channel];
        for (uint32_t frame = 0; frame < count; ++frame) {
            int16_t *sample = &output[frame * 2u + (uint32_t)channel];
            *sample = (int16_t)bridge_sample(*sample,
                delta, frame, count);
        }
    }
}

static void apply_fade(DcAudioOutput *state, int16_t *output,
    uint32_t frames)
{
    for (uint32_t frame = 0; frame < frames && state->fade_remaining > 0u;
         ++frame, --state->fade_remaining) {
        const float gain = 1.0f - (float)state->fade_remaining /
            (float)(state->fade_total > 0u ? state->fade_total : 1u);
        output[frame * 2u] = (int16_t)((float)output[frame * 2u] * gain);
        output[frame * 2u + 1u] =
            (int16_t)((float)output[frame * 2u + 1u] * gain);
    }
}

void dc_audio_output_reset(DcAudioOutput *state)
{
    if (!state) return;
    memset(state, 0, sizeof *state);
    state->average_queue = -1.0;
    state->rate_adjust = 0.0;
}

void dc_audio_output_begin_fade(DcAudioOutput *state, uint32_t frames)
{
    if (!state) return;
    state->fade_remaining = frames;
    state->fade_total = frames;
}

int dc_audio_output_process(DcAudioOutput *state,
    const DcAudioOutputConfig *config,
    const int16_t *input,
    uint32_t frames,
    uint32_t queued_frames,
    int deliver,
    int16_t *output,
    uint32_t output_capacity)
{
    if (!valid_state(state) || !valid_config(config) ||
        (deliver != 0 && deliver != 1) || frames > DC_AUDIO_MAX_INPUT_FRAMES)
        return -1;
    if (frames == 0u) return 0;
    if (!input || (deliver && !output) ||
        output_capacity < required_capacity(frames, config->drc != 0))
        return -1;

    if (!config->drc) {
        if (deliver) {
            memmove(output, input, (size_t)frames * 2u * sizeof *output);
            if (state->gap) {
                apply_gap(state, output, frames);
                state->gap = 0;
            }
            apply_fade(state, output, frames);
        } else {
            state->gap = 1;
        }

        state->position = 0.0;
        state->rate_adjust = 0.0;
        state->previous[0] = input[(frames - 1u) * 2u];
        state->previous[1] = input[(frames - 1u) * 2u + 1u];
        if (deliver) {
            state->last_output[0] = output[(frames - 1u) * 2u];
            state->last_output[1] = output[(frames - 1u) * 2u + 1u];
        }
        return (int)frames;
    }

    const double max_adjust = effective_adjust(config->max_adjust);
    const double average_queue = state->average_queue < 0.0 ?
        (double)queued_frames :
        state->average_queue * 0.95 + (double)queued_frames * 0.05;
    const double target = (double)config->target_frames;
    double adjustment = (target - average_queue) / target * max_adjust * 2.0;
    if (adjustment > max_adjust) adjustment = max_adjust;
    if (adjustment < -max_adjust) adjustment = -max_adjust;
    state->rate_adjust = adjustment;

    const double step = 1.0 / (1.0 + adjustment);
    double end_position = 0.0;
    uint32_t output_frames = 0;
    if (!(step > 0.0) || !isfinite(step) ||
        !count_resampled(state->position, frames, step, &output_frames,
            &end_position) || output_frames > output_capacity)
        return -1;

    if (deliver) {
        double position = state->position;
        for (uint32_t frame = 0; frame < output_frames; ++frame) {
            const int index = (int)floor(position);
            const double fraction = position - (double)index;
            for (int channel = 0; channel < 2; ++channel) {
                const double first = index < 0 ?
                    (double)state->previous[channel] :
                    (double)input[(uint32_t)index * 2u + (uint32_t)channel];
                const double second = (double)input[
                    ((uint32_t)(index + 1) * 2u) + (uint32_t)channel];
                output[frame * 2u + (uint32_t)channel] =
                    interpolate_sample(first + (second - first) * fraction);
            }
            position += step;
        }
    }

    state->average_queue = average_queue;
    state->position = end_position;
    state->previous[0] = input[(frames - 1u) * 2u];
    state->previous[1] = input[(frames - 1u) * 2u + 1u];

    if (!deliver) {
        state->gap = 1;
        return (int)output_frames;
    }
    if (output_frames == 0u) return 0;

    if (state->gap) {
        apply_gap(state, output, output_frames);
        state->gap = 0;
    }
    apply_fade(state, output, output_frames);
    state->last_output[0] = output[(output_frames - 1u) * 2u];
    state->last_output[1] = output[(output_frames - 1u) * 2u + 1u];
    return (int)output_frames;
}
