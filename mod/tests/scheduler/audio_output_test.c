#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dc_audio_output.h"
#include "test_process.h"

enum { MAX_INPUT_FRAMES = 2048, MAX_OUTPUT_FRAMES = 2153 };

static DcAudioOutputConfig drc_config(uint32_t target, double max_adjust)
{
    DcAudioOutputConfig config;
    config.drc = 1;
    config.target_frames = target;
    config.max_adjust = max_adjust;
    return config;
}

static DcAudioOutputConfig exact_config(void)
{
    DcAudioOutputConfig config;
    config.drc = 0;
    config.target_frames = 0;
    config.max_adjust = 0.0;
    return config;
}

static void test_streaming_linear_chunks(void)
{
    static const int16_t first[] = {
        0, 100, 10, 90, 20, 80, 30, 70
    };
    static const int16_t second[] = {
        40, 60, 50, 50, 60, 40, 70, 30
    };
    static const int16_t expected_first[] = {
        0, 100, 10, 90, 20, 80
    };
    static const int16_t expected_second[] = {
        30, 70, 40, 60, 50, 50, 60, 40
    };
    int16_t output[16];
    DcAudioOutput state;
    const DcAudioOutputConfig config = drc_config(100, 0.05);

    dc_audio_output_reset(&state);
    assert(dc_audio_output_process(&state, &config, first, 4, 100, 1,
        output, 8) == 3);
    assert(memcmp(output, expected_first, sizeof expected_first) == 0);
    assert(dc_audio_output_process(&state, &config, second, 4, 100, 1,
        output, 8) == 4);
    assert(memcmp(output, expected_second, sizeof expected_second) == 0);
}

static void test_one_frame_chunks(void)
{
    static const int16_t samples[] = { 0, 100, 200, 300 };
    const DcAudioOutputConfig config = drc_config(100, 0.05);
    DcAudioOutput state;
    int16_t output[8] = { 0 };

    dc_audio_output_reset(&state);
    assert(dc_audio_output_process(&state, &config, &samples[0], 1, 100, 1,
        output, 4) == 0);
    assert(dc_audio_output_process(&state, &config, &samples[1], 1, 100, 1,
        output, 4) == 1);
    assert(output[0] == 0);
    assert(dc_audio_output_process(&state, &config, &samples[2], 1, 100, 1,
        output, 4) == 1);
    assert(output[0] == 100);
    assert(dc_audio_output_process(&state, &config, &samples[3], 1, 100, 1,
        output, 4) == 1);
    assert(output[0] == 200);
}

static void test_drc_clamp_expansion_and_compression(void)
{
    int16_t input[100 * 2];
    int16_t output_a[110 * 2];
    int16_t output_b[110 * 2];
    DcAudioOutput state;
    const DcAudioOutputConfig wide = drc_config(100, 0.50);
    const DcAudioOutputConfig capped = drc_config(100, 0.05);
    int expanded_wide;
    int expanded_capped;
    int compressed_wide;
    int compressed_capped;

    for (int frame = 0; frame < 100; ++frame) {
        input[frame * 2] = (int16_t)frame;
        input[frame * 2 + 1] = (int16_t)(1000 + frame);
    }

    dc_audio_output_reset(&state);
    expanded_wide = dc_audio_output_process(&state, &wide, input, 100, 0,
        1, output_a, 110);
    dc_audio_output_reset(&state);
    expanded_capped = dc_audio_output_process(&state, &capped, input, 100, 0,
        1, output_b, 110);
    assert(expanded_wide == 104);
    assert(expanded_capped == expanded_wide);

    dc_audio_output_reset(&state);
    compressed_wide = dc_audio_output_process(&state, &wide, input, 100, 200,
        1, output_a, 110);
    dc_audio_output_reset(&state);
    compressed_capped = dc_audio_output_process(&state, &capped, input, 100,
        200, 1, output_b, 110);
    assert(compressed_wide == 95);
    assert(compressed_capped == compressed_wide);
}

static void test_queue_average_response(void)
{
    static const int16_t input[] = { 1, -1, 2, -2, 3, -3, 4, -4 };
    const DcAudioOutputConfig config = drc_config(100, 0.05);
    DcAudioOutput state;

    dc_audio_output_reset(&state);
    assert(dc_audio_output_process(&state, &config, input, 4, 20, 0, NULL,
        7) == 4);
    assert(fabs(state.average_queue - 20.0) < 1e-12);
    assert(dc_audio_output_process(&state, &config, input, 4, 100, 0, NULL,
        7) > 0);
    assert(fabs(state.average_queue - 24.0) < 1e-12);
    assert(dc_audio_output_process(&state, &config, input, 4, 100, 0, NULL,
        7) > 0);
    assert(fabs(state.average_queue - 27.8) < 1e-12);
}

static void test_fixed_phase_under_constant_queue(void)
{
    static const int16_t input[] = { 0, 100, 1, 101, 2, 102, 3, 103 };
    const DcAudioOutputConfig config = drc_config(100, 0.05);
    DcAudioOutput state;
    int16_t output[16];

    dc_audio_output_reset(&state);
    assert(dc_audio_output_process(&state, &config, input, 4, 100, 1,
        output, 8) == 3);
    for (int call = 0; call < 5; ++call) {
        assert(dc_audio_output_process(&state, &config, input, 4, 100, 1,
            output, 8) == 4);
        assert(state.position == -1.0);
    }
}

static void test_no_drc_exact_pcm_and_phase_reset(void)
{
    static const int16_t first[] = { 1, -1, 2, -2, 3, -3 };
    static const int16_t second[] = { 40, -40, 50, -50 };
    const DcAudioOutputConfig config = exact_config();
    DcAudioOutput state;
    int16_t output[8] = { 0 };

    dc_audio_output_reset(&state);
    assert(dc_audio_output_process(&state, &config, first, 3, 0, 1,
        output, 3) == 3);
    assert(memcmp(output, first, sizeof first) == 0);
    assert(state.position == 0.0);
    assert(dc_audio_output_process(&state, &config, second, 2, 0, 1,
        output, 2) == 2);
    assert(memcmp(output, second, sizeof second) == 0);
    assert(state.position == 0.0);
}

static void snapshot_rejection(DcAudioOutput *state,
    const DcAudioOutputConfig *config,
    const int16_t *input,
    uint32_t frames,
    uint32_t queued_frames,
    int deliver,
    int16_t *output,
    uint32_t output_capacity,
    size_t output_samples)
{
    DcAudioOutput before = *state;
    int16_t output_before[220];

    assert(output_samples <= sizeof output_before / sizeof output_before[0]);
    if (output_samples) memcpy(output_before, output, output_samples * sizeof *output);
    assert(dc_audio_output_process(state, config, input, frames, queued_frames,
        deliver, output, output_capacity) == -1);
    assert(memcmp(state, &before, sizeof before) == 0);
    if (output_samples)
        assert(memcmp(output, output_before,
            output_samples * sizeof *output) == 0);
}

static void test_invalid_inputs_and_capacity(void)
{
    static const int16_t input[100 * 2] = { 0 };
    int16_t output[220];
    DcAudioOutput state;
    DcAudioOutputConfig config = drc_config(100, 0.05);

    memset(output, 0x5A, sizeof output);
    dc_audio_output_reset(&state);
    snapshot_rejection(&state, &config, input, 100, 0, 1, output, 106,
        sizeof output / sizeof output[0]);
    snapshot_rejection(&state, &config, NULL, 1, 0, 1, output, 4,
        sizeof output / sizeof output[0]);
    snapshot_rejection(&state, &config, input, 1, 0, 1, NULL, 4, 0);
    snapshot_rejection(&state, &config, input, MAX_INPUT_FRAMES + 1u, 0, 1,
        output, 220, sizeof output / sizeof output[0]);
    snapshot_rejection(&state, &config, input, 1, 0, 2, output, 4,
        sizeof output / sizeof output[0]);

    config.max_adjust = NAN;
    snapshot_rejection(&state, &config, input, 1, 0, 1, output, 4,
        sizeof output / sizeof output[0]);
    config.max_adjust = INFINITY;
    snapshot_rejection(&state, &config, input, 1, 0, 1, output, 4,
        sizeof output / sizeof output[0]);
    config.max_adjust = 0.05;
    config.target_frames = 0;
    snapshot_rejection(&state, &config, input, 1, 0, 1, output, 4,
        sizeof output / sizeof output[0]);

    assert(dc_audio_output_process(NULL, &config, input, 1, 0, 1, output,
        4) == -1);
    assert(dc_audio_output_process(&state, NULL, input, 1, 0, 1, output,
        4) == -1);

    dc_audio_output_reset(&state);
    state.position = NAN;
    config.target_frames = 100;
    snapshot_rejection(&state, &config, input, 1, 0, 1, output, 4,
        sizeof output / sizeof output[0]);
    dc_audio_output_reset(&state);
    state.average_queue = INFINITY;
    snapshot_rejection(&state, &config, input, 1, 0, 1, output, 4,
        sizeof output / sizeof output[0]);
    dc_audio_output_reset(&state);
    state.gap = 2;
    snapshot_rejection(&state, &config, input, 1, 0, 1, output, 4,
        sizeof output / sizeof output[0]);
    dc_audio_output_reset(&state);
    state.fade_remaining = 3;
    state.fade_total = 2;
    snapshot_rejection(&state, &config, input, 1, 0, 1, output, 4,
        sizeof output / sizeof output[0]);
}

static void test_zero_frames_no_state_change(void)
{
    DcAudioOutput state;
    DcAudioOutput before;
    const DcAudioOutputConfig config = exact_config();

    dc_audio_output_reset(&state);
    state.position = -0.25;
    state.average_queue = 12.0;
    state.previous[0] = 11;
    state.previous[1] = -11;
    state.last_output[0] = 22;
    state.last_output[1] = -22;
    state.fade_remaining = 3;
    state.fade_total = 5;
    state.gap = 1;
    before = state;
    assert(dc_audio_output_process(&state, &config, NULL, 0, 123, 0, NULL,
        0) == 0);
    assert(memcmp(&state, &before, sizeof state) == 0);
}

static void test_discard_preserves_delivery_and_fade_state(void)
{
    static const int16_t delivered[] = { 100, 300, 200, 400 };
    static const int16_t discarded[] = { 500, 700, 600, 800 };
    const DcAudioOutputConfig config = exact_config();
    DcAudioOutput state;
    int16_t output[8] = { 0 };
    int16_t last_left;
    int16_t last_right;

    dc_audio_output_reset(&state);
    assert(dc_audio_output_process(&state, &config, delivered, 2, 0, 1,
        output, 2) == 2);
    last_left = state.last_output[0];
    last_right = state.last_output[1];
    dc_audio_output_begin_fade(&state, 4);
    assert(dc_audio_output_process(&state, &config, discarded, 2, 0, 0,
        NULL, 2) == 2);
    assert(state.previous[0] == 600 && state.previous[1] == 800);
    assert(state.last_output[0] == last_left && state.last_output[1] == last_right);
    assert(state.fade_remaining == 4 && state.fade_total == 4);
    assert(state.gap == 1);
}

static void test_gap_rejoin_bridges_last_delivered_sample(void)
{
    static const int16_t first[] = { 100, 200, 300, 400 };
    static const int16_t discarded[] = { 1000, 2000, 1100, 2100 };
    static const int16_t rejoin[] = { 1200, 2200, 1300, 2300 };
    const DcAudioOutputConfig config = exact_config();
    DcAudioOutput state;
    int16_t output[8] = { 0 };

    dc_audio_output_reset(&state);
    assert(dc_audio_output_process(&state, &config, first, 2, 0, 1,
        output, 2) == 2);
    assert(state.last_output[0] == 300 && state.last_output[1] == 400);
    assert(dc_audio_output_process(&state, &config, discarded, 2, 0, 0,
        NULL, 2) == 2);
    assert(dc_audio_output_process(&state, &config, rejoin, 2, 0, 1,
        output, 2) == 2);
    assert(output[0] == 300 && output[1] == 400);
    assert(output[2] == 850 && output[3] == 1400);
    assert(state.gap == 0);
}

static void test_fade_progress_and_stop(void)
{
    static const int16_t input[] = {
        1000, -1000, 1000, -1000, 1000, -1000,
        1000, -1000, 1000, -1000, 1000, -1000
    };
    static const int16_t expected[] = {
        0, 0, 250, -250, 500, -500, 750, -750,
        1000, -1000, 1000, -1000
    };
    const DcAudioOutputConfig config = exact_config();
    DcAudioOutput state;
    int16_t output[12] = { 0 };

    dc_audio_output_reset(&state);
    dc_audio_output_begin_fade(&state, 4);
    assert(dc_audio_output_process(&state, &config, input, 6, 0, 1,
        output, 6) == 6);
    assert(memcmp(output, expected, sizeof expected) == 0);
    assert(state.fade_remaining == 0 && state.fade_total == 4);
    assert(dc_audio_output_process(&state, &config, input, 6, 0, 1,
        output, 6) == 6);
    for (int frame = 0; frame < 6; ++frame) {
        assert(output[frame * 2] == 1000);
        assert(output[frame * 2 + 1] == -1000);
    }
}

static void test_maximum_expansion_capacity(void)
{
    static int16_t input[MAX_INPUT_FRAMES * 2];
    static int16_t output[MAX_OUTPUT_FRAMES * 2];
    int16_t output_before[MAX_OUTPUT_FRAMES * 2];
    const DcAudioOutputConfig config = drc_config(100, 0.05);
    DcAudioOutput state;
    DcAudioOutput before;
    int output_frames;

    for (int frame = 0; frame < MAX_INPUT_FRAMES; ++frame) {
        input[frame * 2] = (int16_t)(frame & 0x3FF);
        input[frame * 2 + 1] = (int16_t)(-frame & 0x3FF);
    }
    memset(output, 0x3C, sizeof output);
    memcpy(output_before, output, sizeof output);

    dc_audio_output_reset(&state);
    before = state;
    assert(dc_audio_output_process(&state, &config, input, MAX_INPUT_FRAMES,
        0, 1, output, MAX_OUTPUT_FRAMES - 1u) == -1);
    assert(memcmp(&state, &before, sizeof state) == 0);
    assert(memcmp(output, output_before, sizeof output) == 0);

    output_frames = dc_audio_output_process(&state, &config, input,
        MAX_INPUT_FRAMES, 0, 1, output, MAX_OUTPUT_FRAMES);
    assert(output_frames > MAX_INPUT_FRAMES);
    assert(output_frames <= MAX_OUTPUT_FRAMES);
}

int main(void)
{
    test_process_init();
    test_streaming_linear_chunks();
    test_one_frame_chunks();
    test_drc_clamp_expansion_and_compression();
    test_queue_average_response();
    test_fixed_phase_under_constant_queue();
    test_no_drc_exact_pcm_and_phase_reset();
    test_invalid_inputs_and_capacity();
    test_zero_frames_no_state_change();
    test_discard_preserves_delivery_and_fade_state();
    test_gap_rejoin_bridges_last_delivered_sample();
    test_fade_progress_and_stop();
    test_maximum_expansion_capacity();
    puts("PASS: audio output streaming, DRC pacing, validation, discard gaps, fades, and capacity bounds");
    return 0;
}
