#include "dc_audio_host.h"
#include "dc_audio_output.h"
#include "spu.h"
#include <SDL.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" void psx_fatal_halt(const char *reason);

static DcAudioOutput output;
static DcAudioOutputConfig config;
static bool configured, was_held, was_burst, hold_enabled, mute_output, log_output;
static uint32_t fade_frames, ahead_frames;
static FILE *dump_file;
static int16_t input[2048 * 2], samples[8192 * 2];
static uint32_t last_device, last_queued_frames, recovery_device, prefill_device;
static uint64_t output_underruns, output_overflow_drops;
static bool timing_enabled, timing_armed, timing_pump_active,
    timing_have_pump_anchor, timing_have_submit_anchor,
    timing_no_pcm_episode;
static uint64_t timing_last_pump_end_ns, timing_last_submit_ns;
static uint64_t timing_sequence;
static uint32_t timing_event_count, timing_event_write;
static uint64_t timing_pump_calls, timing_pump_gap_count,
    timing_pump_gap_total_ns, timing_pump_gap_max_ns, timing_last_pump_gap_ns;
static uint64_t timing_pump_duration_total_ns, timing_pump_duration_max_ns,
    timing_last_pump_duration_ns;
static uint64_t timing_catch_up_duration_total_ns,
    timing_catch_up_duration_max_ns, timing_last_catch_up_duration_ns;
static uint64_t timing_submit_count, timing_submit_duration_total_ns,
    timing_submit_duration_max_ns, timing_last_submit_duration_ns,
    timing_last_submit_gap_ns;
static uint64_t timing_no_pcm_episodes, timing_starvations,
    timing_recovery_starts, timing_recovery_ends;
static uint32_t timing_current_queued_frames,
    timing_current_input_available_frames, timing_current_input_popped_frames,
    timing_current_delivered_frames, timing_current_device_status;
static uint32_t timing_pump_device, timing_pump_queued_before_frames,
    timing_pump_previous_pump_queued_frames,
    timing_pump_previous_submit_queued_frames,
    timing_pump_queued_before_submit_frames,
    timing_pump_input_available_frames,
    timing_pump_input_popped_frames, timing_pump_delivered_frames,
    timing_pump_device_status;
static uint64_t timing_pump_start_ns, timing_pump_gap_ns,
    timing_pump_submit_gap_ns, timing_pump_catch_up_start_ns,
    timing_pump_catch_up_duration_ns, timing_pump_submit_duration_ns;
static uint64_t timing_pump_previous_pump_end_ns,
    timing_pump_previous_submit_end_ns;
static bool timing_pump_have_previous_pump_anchor,
    timing_pump_have_previous_submit_anchor,
    timing_pump_recovery_owned_at_entry;
static uint32_t timing_last_pump_queued_frames,
    timing_last_submit_queued_frames;
static uint32_t timing_pump_event_count, timing_pump_event_indices[4];
static DcAudioHostTimingEvent timing_events[DC_AUDIO_TIMING_EVENT_CAPACITY];
#if defined(DC_AUDIO_HOST_TEST)
static DcAudioHostTestClock timing_test_clock;
static void *timing_test_clock_opaque;
#endif

static void fail(const char *message)
{
    psx_fatal_halt(message);
    std::abort();
}

static int setting(const char *name, int fallback, int low, int high)
{
    const char *value = std::getenv(name);
    long result = value ? std::strtol(value, nullptr, 10) : fallback;
    return (int)std::clamp(result, (long)low, (long)high);
}

static uint64_t timing_now_ns(void)
{
#if defined(DC_AUDIO_HOST_TEST)
    if (timing_test_clock) return timing_test_clock(timing_test_clock_opaque);
#endif
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

static uint64_t timing_elapsed(uint64_t now, uint64_t before)
{
    return now >= before ? now - before : 0;
}

static void timing_disarm(void)
{
    timing_armed = false;
    timing_pump_active = false;
    timing_have_pump_anchor = false;
    timing_have_submit_anchor = false;
    timing_no_pcm_episode = false;
}

static void timing_reset_data(void)
{
    timing_disarm();
    timing_last_pump_end_ns = timing_last_submit_ns = 0;
    timing_sequence = 0;
    timing_event_count = timing_event_write = 0;
    timing_pump_calls = timing_pump_gap_count = timing_pump_gap_total_ns = 0;
    timing_pump_gap_max_ns = timing_last_pump_gap_ns = 0;
    timing_pump_duration_total_ns = timing_pump_duration_max_ns = 0;
    timing_last_pump_duration_ns = 0;
    timing_catch_up_duration_total_ns = timing_catch_up_duration_max_ns = 0;
    timing_last_catch_up_duration_ns = 0;
    timing_submit_count = timing_submit_duration_total_ns = 0;
    timing_submit_duration_max_ns = timing_last_submit_duration_ns = 0;
    timing_last_submit_gap_ns = 0;
    timing_no_pcm_episodes = timing_starvations = 0;
    timing_recovery_starts = timing_recovery_ends = 0;
    timing_current_queued_frames = 0;
    timing_current_input_available_frames = 0;
    timing_current_input_popped_frames = 0;
    timing_current_delivered_frames = 0;
    timing_current_device_status = 0;
    timing_last_pump_queued_frames = timing_last_submit_queued_frames = 0;
    timing_pump_event_count = 0;
    timing_pump_recovery_owned_at_entry = false;
    std::memset(timing_events, 0, sizeof timing_events);
}

static void timing_begin_pump(uint32_t device)
{
    if (!timing_enabled || !device) return;
    timing_armed = true;
    timing_pump_active = true;
    timing_pump_device = device;
    timing_pump_start_ns = timing_now_ns();
    timing_pump_gap_ns = timing_have_pump_anchor ?
        timing_elapsed(timing_pump_start_ns, timing_last_pump_end_ns) : 0;
    timing_pump_submit_gap_ns = timing_have_submit_anchor ?
        timing_elapsed(timing_pump_start_ns, timing_last_submit_ns) : 0;
    timing_pump_have_previous_pump_anchor = timing_have_pump_anchor;
    timing_pump_previous_pump_end_ns = timing_last_pump_end_ns;
    timing_pump_have_previous_submit_anchor = timing_have_submit_anchor;
    timing_pump_previous_submit_end_ns = timing_last_submit_ns;
    timing_pump_recovery_owned_at_entry = recovery_device != 0;
    timing_pump_previous_pump_queued_frames = timing_have_pump_anchor ?
        timing_last_pump_queued_frames : 0;
    timing_pump_previous_submit_queued_frames = timing_have_submit_anchor ?
        timing_last_submit_queued_frames : 0;
    timing_pump_queued_before_frames =
        SDL_GetQueuedAudioSize(device) / (sizeof(int16_t) * 2u);
    timing_pump_queued_before_submit_frames =
        timing_pump_queued_before_frames;
    timing_pump_input_available_frames = 0;
    timing_pump_input_popped_frames = 0;
    timing_pump_delivered_frames = 0;
    timing_pump_catch_up_start_ns = 0;
    timing_pump_catch_up_duration_ns = 0;
    timing_pump_submit_duration_ns = 0;
    timing_pump_event_count = 0;
    timing_pump_device_status = (uint32_t)SDL_GetAudioDeviceStatus(device);
    timing_current_queued_frames = timing_pump_queued_before_frames;
    timing_current_device_status = timing_pump_device_status;
    timing_last_pump_gap_ns = timing_pump_gap_ns;
    timing_last_submit_gap_ns = timing_pump_submit_gap_ns;
    timing_pump_calls++;
    if (timing_have_pump_anchor) {
        timing_pump_gap_count++;
        timing_pump_gap_total_ns += timing_pump_gap_ns;
        if (timing_pump_gap_ns > timing_pump_gap_max_ns)
            timing_pump_gap_max_ns = timing_pump_gap_ns;
    }
}

static void timing_finish_catch_up(uint64_t finish_ns)
{
    if (!timing_enabled || !timing_pump_active) return;
    const uint64_t duration = timing_elapsed(finish_ns,
                                             timing_pump_catch_up_start_ns);
    timing_pump_catch_up_duration_ns = duration;
    timing_last_catch_up_duration_ns = duration;
    timing_catch_up_duration_total_ns += duration;
    if (duration > timing_catch_up_duration_max_ns)
        timing_catch_up_duration_max_ns = duration;
}

static void timing_record(uint32_t kind, uint32_t queued_after,
                          uint32_t device_status, uint32_t recovery_owned)
{
    if (!timing_enabled || !timing_armed || !timing_pump_active) return;
    const uint64_t now = timing_now_ns();
    DcAudioHostTimingEvent *event = &timing_events[timing_event_write];
    std::memset(event, 0, sizeof *event);
    event->sequence = ++timing_sequence;
    event->kind = kind;
    event->device = timing_pump_device;
    event->host_ns = now;
    event->since_previous_pump_ns = timing_pump_have_previous_pump_anchor ?
        timing_elapsed(now, timing_pump_previous_pump_end_ns) : 0;
    event->since_previous_submit_ns = timing_pump_have_previous_submit_anchor ?
        timing_elapsed(now, timing_pump_previous_submit_end_ns) : 0;
    event->pump_duration_ns = 0;
    event->catch_up_duration_ns = timing_pump_catch_up_duration_ns;
    event->queue_submit_duration_ns = timing_pump_submit_duration_ns;
    event->previous_pump_queued_frames =
        timing_pump_previous_pump_queued_frames;
    event->previous_submit_queued_frames =
        timing_pump_previous_submit_queued_frames;
    event->queued_before_frames = timing_pump_queued_before_submit_frames;
    event->queued_after_frames = queued_after;
    event->input_available_frames = timing_pump_input_available_frames;
    event->input_popped_frames = timing_pump_input_popped_frames;
    event->delivered_frames = timing_pump_delivered_frames;
    event->device_status = device_status;
    event->recovery_owned = recovery_owned;
    if (timing_pump_event_count < sizeof timing_pump_event_indices /
                                  sizeof timing_pump_event_indices[0])
        timing_pump_event_indices[timing_pump_event_count++] = timing_event_write;
    timing_event_write = (timing_event_write + 1u) %
        DC_AUDIO_TIMING_EVENT_CAPACITY;
    if (timing_event_count < DC_AUDIO_TIMING_EVENT_CAPACITY)
        timing_event_count++;
    if (kind == DC_AUDIO_TIMING_NO_PCM_EMPTY) timing_no_pcm_episodes++;
    if (kind == DC_AUDIO_TIMING_STARVATION) timing_starvations++;
    if (kind == DC_AUDIO_TIMING_RECOVERY_START) timing_recovery_starts++;
    if (kind == DC_AUDIO_TIMING_RECOVERY_END) timing_recovery_ends++;
}

static void timing_finish_pump(uint32_t device)
{
    if (!timing_enabled || !timing_pump_active) return;
    const uint64_t now = timing_now_ns();
    const uint64_t duration = timing_elapsed(now, timing_pump_start_ns);
    timing_last_pump_duration_ns = duration;
    timing_pump_duration_total_ns += duration;
    if (duration > timing_pump_duration_max_ns)
        timing_pump_duration_max_ns = duration;
    timing_current_queued_frames =
        SDL_GetQueuedAudioSize(device) / (sizeof(int16_t) * 2u);
    timing_current_device_status = (uint32_t)SDL_GetAudioDeviceStatus(device);
    timing_current_input_available_frames = timing_pump_input_available_frames;
    timing_current_input_popped_frames = timing_pump_input_popped_frames;
    timing_current_delivered_frames = timing_pump_delivered_frames;
    for (uint32_t i = 0; i < timing_pump_event_count; ++i) {
        DcAudioHostTimingEvent *event =
            &timing_events[timing_pump_event_indices[i]];
        event->pump_duration_ns = duration;
        event->catch_up_duration_ns = timing_pump_catch_up_duration_ns;
        event->queue_submit_duration_ns = timing_pump_submit_duration_ns;
        event->queued_after_frames = timing_current_queued_frames;
        event->input_available_frames = timing_pump_input_available_frames;
        event->input_popped_frames = timing_pump_input_popped_frames;
        event->delivered_frames = timing_pump_delivered_frames;
    }
    timing_pump_event_count = 0;
    timing_last_pump_queued_frames = timing_current_queued_frames;
    const bool externally_paused =
        timing_pump_device_status == SDL_AUDIO_PAUSED &&
        !timing_pump_recovery_owned_at_entry;
    timing_last_pump_end_ns = now;
    timing_have_pump_anchor = true;
    timing_pump_active = false;
    if (externally_paused) timing_disarm();
}

static void timing_note_submit(uint32_t device, uint64_t start_ns,
                               uint64_t finish_ns)
{
    if (!timing_enabled || !timing_armed || !timing_pump_active) return;
    const uint64_t duration = timing_elapsed(finish_ns, start_ns);
    timing_pump_submit_duration_ns = duration;
    timing_last_submit_duration_ns = duration;
    timing_submit_count++;
    timing_submit_duration_total_ns += duration;
    if (duration > timing_submit_duration_max_ns)
        timing_submit_duration_max_ns = duration;
    timing_last_submit_ns = finish_ns;
    timing_have_submit_anchor = true;
    timing_current_queued_frames =
        SDL_GetQueuedAudioSize(device) / (sizeof(int16_t) * 2u);
    timing_last_submit_queued_frames = timing_current_queued_frames;
}

static void timing_disarm_for_intentional_gap(void)
{
    if (timing_enabled) timing_disarm();
}

static void configure(void)
{
    if (configured) return;
    config.drc = setting("DC_AUDIO_DRC", 1, 0, 1);
    config.target_frames = (uint32_t)setting("DC_AUDIO_TARGET_MS", 60, 20, 180) * 441u / 10u;
    const char *adjust = std::getenv("DC_AUDIO_DRC_MAX");
    double maximum = adjust ? std::strtod(adjust, nullptr) : 0.01;
    config.max_adjust = std::isfinite(maximum) ? std::clamp(maximum, 0.0, 0.05) : 0.01;
    fade_frames = (uint32_t)setting("DC_MENU_AUDIO_FADEIN_MS", 40, 0, 10000) * 441u / 10u;
    ahead_frames = (uint32_t)setting("DC_AUDIO_AHEAD_MS", 30, 0, 1000) * 441u / 10u;
    hold_enabled = setting("DC_MENU_AUDIO_HOLD", 1, 0, 1) != 0;
    mute_output = setting("DC_AUDIO_MUTE_OUT", 0, 0, 1) != 0;
    log_output = setting("DC_AUDIO_LOG", 0, 0, 1) != 0;
    timing_enabled = setting("DC_AUDIO_TIMING", 0, 0, 1) != 0;
    const char *dump = std::getenv("DC_AUDIO_WAV");
    if (dump && *dump) {
        dump_file = std::fopen(dump, "wb");
        if (!dump_file) fail("Audio: could not open output recording");
    }
    dc_audio_output_reset(&output);
    timing_reset_data();
    configured = true;
}

static void queue(uint32_t device, int16_t *data, uint32_t frames)
{
    if (!frames) return;
    if (mute_output) std::memset(data, 0, frames * 2u * sizeof(int16_t));
    if (dump_file && (std::fwrite(data, sizeof(int16_t) * 2u, frames, dump_file) != frames ||
                      std::fflush(dump_file))) fail("Audio: output recording failed");
    const bool measure_submit = timing_enabled && timing_armed &&
        timing_pump_active;
    const uint64_t submit_start = measure_submit ? timing_now_ns() : 0;
    if (SDL_QueueAudio(device, data, frames * 2u * sizeof(int16_t)) < 0)
        fail("Audio: could not queue device samples");
    if (measure_submit) timing_note_submit(device, submit_start, timing_now_ns());
}

static void finish_recovery(void)
{
    if (recovery_device) {
        if (timing_enabled && timing_armed && timing_pump_active) {
            const uint32_t queued = SDL_GetQueuedAudioSize(recovery_device) /
                (sizeof(int16_t) * 2u);
            const uint32_t status =
                (uint32_t)SDL_GetAudioDeviceStatus(recovery_device);
            timing_record(DC_AUDIO_TIMING_RECOVERY_END, queued, status, 1);
        }
        SDL_PauseAudioDevice(recovery_device, 0);
    }
    recovery_device = 0;
}

void dc_audio_host_reset(uint32_t device)
{
    timing_disarm_for_intentional_gap();
    finish_recovery();
    prefill_device = 0;
    timing_reset_data();
    dc_audio_output_reset(&output);
    was_held = was_burst = false;
    spu_output_flush();
    if (device) SDL_ClearQueuedAudio(device);
    last_device = device;
    prefill_device = device;
    last_queued_frames = 0;
    if (configured) dc_audio_output_begin_fade(&output, fade_frames);
}

void dc_audio_host_shutdown(void)
{
    timing_disarm_for_intentional_gap();
    finish_recovery();
    prefill_device = 0;
    if (dump_file) {
        int result = std::fclose(dump_file);
        dump_file = nullptr;
        if (result) fail("Audio: could not finish output recording");
    }
    configured = was_held = was_burst = false;
    timing_reset_data();
    dc_audio_output_reset(&output);
    last_device = 0;
    last_queued_frames = 0;
}

void dc_audio_host_pump(uint32_t device, int held, int burst, int xa_active)
{
    configure();
    if (device && last_device != device) {
        if (last_device) finish_recovery();
        prefill_device = device;
        if (timing_enabled) timing_disarm_for_intentional_gap();
    }
    if (!config.drc) prefill_device = 0;
    const bool resume = was_held || was_burst;
    const bool intentional = burst || !device || (held && hold_enabled) || resume;
    last_device = device;
    if (intentional) timing_disarm_for_intentional_gap();
    else timing_begin_pump(device);
    if (timing_pump_active) timing_pump_catch_up_start_ns = timing_now_ns();
    spu_catch_up();
    if (timing_pump_active) timing_finish_catch_up(timing_now_ns());
    if (burst || !device) {
        prefill_device = 0;
        finish_recovery();
        spu_output_flush();
        if (device) SDL_ClearQueuedAudio(device);
        dc_audio_output_reset(&output);
        was_burst = burst != 0;
        last_queued_frames = 0;
        return;
    }
    if (held && hold_enabled) {
        prefill_device = 0;
        finish_recovery();
        if (!was_held) {
            SDL_ClearQueuedAudio(device);
            uint32_t frames = spu_output_pop(input, 735);
            if (!frames) {
                frames = 735;
                for (uint32_t i = 0; i < frames; ++i) {
                    input[i * 2] = output.last_output[0];
                    input[i * 2 + 1] = output.last_output[1];
                }
            }
            for (uint32_t i = 0; i < frames; ++i) {
                float gain = 1.0f - (float)(i + 1) / frames;
                input[i * 2] = (int16_t)(input[i * 2] * gain);
                input[i * 2 + 1] = (int16_t)(input[i * 2 + 1] * gain);
            }
            queue(device, input, frames);
        }
        spu_output_flush();
        was_held = true;
        last_queued_frames = SDL_GetQueuedAudioSize(device) / (sizeof(int16_t) * 2u);
        return;
    }
    if (was_held || was_burst) {
        timing_disarm_for_intentional_gap();
        dc_audio_output_reset(&output);
        dc_audio_output_begin_fade(&output, fade_frames);
        was_held = was_burst = false;
        prefill_device = device;
        timing_begin_pump(device);
    }
    const uint32_t available = spu_output_available();
    if (timing_pump_active) timing_pump_input_available_frames = available;
    uint32_t frames = available;
    if (!frames) {
        last_queued_frames = SDL_GetQueuedAudioSize(device) /
            (sizeof(int16_t) * 2u);
        if (prefill_device == device && config.drc &&
            last_queued_frames >= config.target_frames) {
            if (recovery_device == device) finish_recovery();
            prefill_device = 0;
        }
        if (timing_pump_active) {
            timing_pump_queued_before_submit_frames = last_queued_frames;
            if (last_queued_frames == 0 &&
                timing_pump_device_status == SDL_AUDIO_PLAYING) {
                if (!timing_no_pcm_episode) {
                    timing_record(DC_AUDIO_TIMING_NO_PCM_EMPTY, 0,
                                  timing_pump_device_status, 0);
                    timing_no_pcm_episode = true;
                }
            } else {
                timing_no_pcm_episode = false;
            }
            timing_finish_pump(device);
        }
        return;
    }
    timing_no_pcm_episode = false;
    if (frames > 2048) {
        output_overflow_drops += frames - 2048;
        spu_output_drop(frames - 2048);
        output.position = 0;
        output.gap = 1;
        frames = 2048;
    }
    frames = spu_output_pop(input, frames);
    if (timing_pump_active) timing_pump_input_popped_frames = frames;
    uint32_t queued = SDL_GetQueuedAudioSize(device) / (sizeof(int16_t) * 2u);
    if (timing_pump_active)
        timing_pump_queued_before_submit_frames = queued;
    const bool prefill = prefill_device == device;
    const bool prefill_shortfall = prefill && config.drc &&
        queued < config.target_frames;
    uint32_t status = SDL_AUDIO_STOPPED;
    if (config.drc && (!queued || prefill_shortfall))
        status = (uint32_t)SDL_GetAudioDeviceStatus(device);
    if (timing_pump_active && (!queued || prefill_shortfall))
        timing_pump_device_status = status;
    if (config.drc && !queued) {
        output_underruns++;
        if (status == SDL_AUDIO_PLAYING) {
            if (timing_pump_active)
                timing_record(DC_AUDIO_TIMING_STARVATION, queued, status, 0);
            if (prefill) {
                SDL_PauseAudioDevice(device, 1);
                recovery_device = device;
                if (timing_pump_active)
                    timing_record(DC_AUDIO_TIMING_RECOVERY_START, queued,
                                  (uint32_t)SDL_GetAudioDeviceStatus(device), 1);
                output.last_output[0] = 0;
                output.last_output[1] = 0;
                output.gap = 1;
            }
        } else if (prefill && recovery_device == device) {
            output.last_output[0] = 0;
            output.last_output[1] = 0;
            output.gap = 1;
        }
    } else if (prefill_shortfall && status == SDL_AUDIO_PLAYING &&
               recovery_device == 0) {
        SDL_PauseAudioDevice(device, 1);
        recovery_device = device;
        if (timing_pump_active)
            timing_record(DC_AUDIO_TIMING_RECOVERY_START, queued,
                          (uint32_t)SDL_GetAudioDeviceStatus(device), 1);
    }
    bool deliver = queued <= 8820u && !(config.drc && ahead_frames && !xa_active &&
                                      queued > config.target_frames + ahead_frames);
    int count = dc_audio_output_process(&output, &config, input, frames, queued,
                                       deliver, samples, 8192);
    if (count < 0) fail("Audio: invalid output conversion state");
    if (timing_pump_active)
        timing_pump_delivered_frames = deliver ? (uint32_t)count : 0;
    if (log_output)
        std::fprintf(stderr, "audio: guest=%u queued=%u output=%d held=%d burst=%d\n",
                     frames, queued, deliver ? count : 0, held, burst);
    last_queued_frames = queued;
    if (deliver) queue(device, samples, (uint32_t)count);
    if (prefill_device == device && config.drc) {
        const uint32_t queued_after = SDL_GetQueuedAudioSize(device) /
            (sizeof(int16_t) * 2u);
        if (queued_after >= config.target_frames) {
            if (recovery_device == device) finish_recovery();
            prefill_device = 0;
        }
    }
    timing_finish_pump(device);
}

extern "C" int psx_audio_out_stats(double *fill_ms, double *target_ms,
                                    uint64_t *underruns,
                                    uint64_t *overflow_drops, double *correction,
                                    int *legacy, int *host_rate) {
    if (!fill_ms || !target_ms || !underruns || !overflow_drops ||
        !correction || !legacy || !host_rate) return 0;
    *fill_ms = (double)last_queued_frames * 1000.0 / 44100.0;
    *target_ms = configured ? (double)config.target_frames * 1000.0 / 44100.0 : 0.0;
    *underruns = output_underruns;
    *overflow_drops = output_overflow_drops;
    *correction = configured ? output.rate_adjust : 0.0;
    *legacy = 0;
    *host_rate = 44100;
    return configured && last_device != 0;
}

extern "C" int dc_audio_host_timing_snapshot(
    DcAudioHostTimingSnapshot *snapshot)
{
    if (!snapshot) return 0;
    std::memset(snapshot, 0, sizeof *snapshot);
    if (!timing_enabled || !configured || !last_device) return 0;
    snapshot->enabled = 1;
    snapshot->event_count = timing_event_count;
    snapshot->event_sequence = timing_sequence;
    snapshot->pump_calls = timing_pump_calls;
    snapshot->pump_gap_count = timing_pump_gap_count;
    snapshot->pump_gap_total_ns = timing_pump_gap_total_ns;
    snapshot->pump_gap_max_ns = timing_pump_gap_max_ns;
    snapshot->last_pump_gap_ns = timing_last_pump_gap_ns;
    snapshot->pump_duration_total_ns = timing_pump_duration_total_ns;
    snapshot->pump_duration_max_ns = timing_pump_duration_max_ns;
    snapshot->last_pump_duration_ns = timing_last_pump_duration_ns;
    snapshot->catch_up_duration_total_ns = timing_catch_up_duration_total_ns;
    snapshot->catch_up_duration_max_ns = timing_catch_up_duration_max_ns;
    snapshot->last_catch_up_duration_ns = timing_last_catch_up_duration_ns;
    snapshot->submit_count = timing_submit_count;
    snapshot->submit_duration_total_ns = timing_submit_duration_total_ns;
    snapshot->submit_duration_max_ns = timing_submit_duration_max_ns;
    snapshot->last_submit_duration_ns = timing_last_submit_duration_ns;
    snapshot->last_submit_gap_ns = timing_last_submit_gap_ns;
    snapshot->no_pcm_episodes = timing_no_pcm_episodes;
    snapshot->starvations = timing_starvations;
    snapshot->recovery_starts = timing_recovery_starts;
    snapshot->recovery_ends = timing_recovery_ends;
    snapshot->current_queued_frames = timing_current_queued_frames;
    snapshot->current_input_available_frames =
        timing_current_input_available_frames;
    snapshot->current_input_popped_frames = timing_current_input_popped_frames;
    snapshot->current_delivered_frames = timing_current_delivered_frames;
    snapshot->current_device_status = timing_current_device_status;
    snapshot->recovery_owned = recovery_device != 0;
    const uint32_t start = timing_event_count == DC_AUDIO_TIMING_EVENT_CAPACITY ?
        timing_event_write : 0;
    for (uint32_t i = 0; i < timing_event_count; ++i)
        snapshot->events[i] = timing_events[(start + i) %
                                             DC_AUDIO_TIMING_EVENT_CAPACITY];
    return 1;
}

#if defined(DC_AUDIO_HOST_TEST)
extern "C" void dc_audio_host_test_set_clock(DcAudioHostTestClock clock,
                                               void *opaque)
{
    timing_test_clock = clock;
    timing_test_clock_opaque = opaque;
}
#endif
