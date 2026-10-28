#define main dc_spu_pages_backend_main
#include "spu_pages_backend_test.c"
#undef main
#include "dc_audio_host.h"
#include <SDL.h>
#include <stdio.h>

extern int psx_audio_out_stats(double *fill_ms, double *target_ms,
                               uint64_t *underruns, uint64_t *overflow_drops,
                               double *correction, int *legacy, int *host_rate);

static void setup_audio(uint32_t device)
{
    cycle = 0; spu_init(); dc_audio_host_reset(device);
    seed(spu_ram, 0x11); start(0, 0);
    spu_write(0x1F801C00u, 0x3FFF); spu_write(0x1F801C02u, 0x3FFF);
    spu_write(0x1F801C04u, 0x1000); spu_write(0x1F801D80u, 0x3FFF);
    spu_write(0x1F801D82u, 0x3FFF); spu_write(0x1F801DAAu, 0x8001);
    voices[0].env_level = 0x7FFF; voices[0].adsr_phase = ADSR_SUSTAIN;
    int16_t cd[512];
    for (unsigned i = 0; i < 512; ++i) cd[i] = (int16_t)(100 + i);
    spu_cd_audio_push(cd, 256);
}

static void set_audio_env(const char *name, const char *value)
{
#if defined(_WIN32)
    assert(_putenv_s(name, value) == 0);
#else
    assert(SDL_setenv(name, value, 1) == 0);
#endif
}

static void assert_guest_state(const uint8_t *core, uint32_t size,
                               const uint8_t *ram, const uint8_t *cd, uint32_t cd_size)
{
    uint8_t *current = malloc(size), *current_cd = malloc(cd_size); assert(current && current_cd);
    spu_snapshot_core_write(current);
    assert(!memcmp(core, current, size));
    assert(!memcmp(ram, spu_ram, SPU_RAM_SIZE));
    assert(spu_cd_snapshot_bytes() == cd_size);
    spu_cd_snapshot_write(current_cd); assert(!memcmp(cd, current_cd, cd_size));
    assert(!spu_output_available());
    free(current); free(current_cd);
}

static void assert_guest_state_without_output(const uint8_t *core, uint32_t size,
                                              const uint8_t *ram, const uint8_t *cd,
                                              uint32_t cd_size)
{
    uint8_t *current = malloc(size), *current_cd = malloc(cd_size);
    assert(current && current_cd);
    spu_snapshot_core_write(current);
    assert(!memcmp(core, current, size));
    assert(!memcmp(ram, spu_ram, SPU_RAM_SIZE));
    assert(spu_cd_snapshot_bytes() == cd_size);
    spu_cd_snapshot_write(current_cd); assert(!memcmp(cd, current_cd, cd_size));
    free(current); free(current_cd);
}

static void generate_audio_frames(uint32_t frames)
{
    cycle += 768u * frames;
    spu_catch_up();
}

static void assert_audio_status(uint32_t device, SDL_AudioStatus expected)
{
    assert(SDL_GetAudioDeviceStatus(device) == expected);
}

static void assert_real_pcm_dump(const char *path)
{
    FILE *file = fopen(path, "rb");
    assert(file);
    assert(fseek(file, 0, SEEK_END) == 0);
    long bytes = ftell(file);
    assert(bytes >= 4 && (bytes % 4) == 0);
    assert(bytes < (long)(2646u * 4u));
    assert(fseek(file, 0, SEEK_SET) == 0);

    int saw_nonzero = 0;
    int16_t samples[256];
    size_t read_count;
    while ((read_count = fread(samples, sizeof(samples[0]),
                               sizeof(samples) / sizeof(samples[0]), file)) != 0) {
        for (size_t i = 0; i + 1 < read_count; i += 2) {
            if (samples[i] != 0 || samples[i + 1] != 0) {
                saw_nonzero = 1;
                break;
            }
        }
        if (saw_nonzero) break;
    }
    assert(saw_nonzero);
    fclose(file);
}

static void assert_files_equal(const char *left_path, const char *right_path)
{
    FILE *left = fopen(left_path, "rb"), *right = fopen(right_path, "rb");
    assert(left && right);
    assert(fseek(left, 0, SEEK_END) == 0);
    assert(fseek(right, 0, SEEK_END) == 0);
    long left_size = ftell(left), right_size = ftell(right);
    assert(left_size >= 0 && left_size == right_size);
    assert(fseek(left, 0, SEEK_SET) == 0);
    assert(fseek(right, 0, SEEK_SET) == 0);
    uint8_t left_buf[4096], right_buf[4096];
    while (left_size > 0) {
        size_t want = (size_t)(left_size < (long)sizeof left_buf ?
                               left_size : (long)sizeof left_buf);
        assert(fread(left_buf, 1, want, left) == want);
        assert(fread(right_buf, 1, want, right) == want);
        assert(!memcmp(left_buf, right_buf, want));
        left_size -= (long)want;
    }
    fclose(left); fclose(right);
}

static void start_host_owned_pause(uint32_t device)
{
    dc_audio_host_shutdown();
    setup_audio(device);
    SDL_PauseAudioDevice(device, 0);
    generate_audio_frames(735);
    dc_audio_host_pump(device, 0, 0, 0);
    assert_audio_status(device, SDL_AUDIO_PAUSED);
}

static void test_host_independent_guest_time(uint32_t device)
{
    setup_audio(device); cycle = 768u * 100u + 31u; spu_catch_up();
    uint32_t size = spu_snapshot_core_bytes(), cd_size = spu_cd_snapshot_bytes();
    uint8_t *core = malloc(size), *ram = malloc(SPU_RAM_SIZE), *cd = malloc(cd_size);
    assert(core && ram && cd); spu_snapshot_core_write(core);
    memcpy(ram, spu_ram, SPU_RAM_SIZE); spu_cd_snapshot_write(cd);
    for (unsigned mode = 0; mode < 5; ++mode) {
        dc_audio_host_shutdown(); setup_audio(device);
        if (mode == 1) {
            int16_t silence[9000 * 2] = {0};
            assert(SDL_QueueAudio(device, silence, sizeof silence) == 0);
        }
        cycle = 768u * 100u + 31u;
        dc_audio_host_pump(mode == 2 ? 0 : device, mode == 3, mode == 4, 0);
        assert_guest_state(core, size, ram, cd, cd_size);
        if (mode == 1) assert(SDL_GetQueuedAudioSize(device) == 9000u * 4u);
        if (mode == 2 || mode == 4) assert(!SDL_GetQueuedAudioSize(device));
    }
    free(core); free(ram); free(cd);
}

static void test_hold_and_reset(uint32_t device)
{
    dc_audio_host_shutdown(); setup_audio(device);
    cycle = 768u * 100u; dc_audio_host_pump(device, 0, 0, 0);
    uint32_t size = spu_snapshot_core_bytes(); uint8_t *before = malloc(size), *after = malloc(size);
    assert(before && after); spu_snapshot_core_write(before);
    dc_audio_host_pump(device, 1, 0, 0);
    uint32_t tail = SDL_GetQueuedAudioSize(device); assert(tail == 735u * 4u);
    dc_audio_host_pump(device, 1, 0, 0); assert(SDL_GetQueuedAudioSize(device) == tail);
    spu_snapshot_core_write(after); assert(!memcmp(before, after, size));
    dc_audio_host_pump(device, 0, 0, 0); assert(SDL_GetQueuedAudioSize(device) == tail);
    cycle += 768u * 100u; dc_audio_host_pump(device, 0, 0, 0);
    assert(SDL_GetQueuedAudioSize(device) > tail);
    spu_snapshot_core_write(before); dc_audio_host_reset(device);
    spu_snapshot_core_write(after); assert(!memcmp(before, after, size));
    assert(!SDL_GetQueuedAudioSize(device) && !spu_output_available());
    free(before); free(after);
}

static void test_output_backlog(uint32_t device)
{
    dc_audio_host_shutdown(); setup_audio(device);
    cycle = 768u * 3000u; spu_catch_up(); assert(spu_output_available() == 3000);
    dc_audio_host_pump(device, 0, 0, 1);
    assert(!spu_output_available());
    assert(SDL_GetQueuedAudioSize(device) > 0 && SDL_GetQueuedAudioSize(device) <= (7938u + 2153u) * 4u);
}

static void test_empty_queue_recovery(uint32_t device)
{
    char path[1024];
    char *base = SDL_GetBasePath();
    assert(base);
    int path_length = snprintf(path, sizeof path,
                               "%sdc_audio_host_recovery_test.s16le", base);
    SDL_free(base);
    assert(path_length > 0 && (size_t)path_length < sizeof path);
    remove(path);
    dc_audio_host_shutdown();
    set_audio_env("DC_AUDIO_DRC", "1");
    set_audio_env("DC_AUDIO_DRC_MAX", "0.01");
    set_audio_env("DC_AUDIO_TARGET_MS", "60");
    set_audio_env("DC_AUDIO_AHEAD_MS", "30");
    set_audio_env("DC_AUDIO_WAV", path);
    set_audio_env("DC_MENU_AUDIO_FADEIN_MS", "0");
    setup_audio(device);
    SDL_PauseAudioDevice(device, 0);
    spu_write(0x1F801DB0u, 0x7FFF);
    spu_write(0x1F801DB2u, 0x7FFF);
    int16_t cd_recovery[512 * 2];
    for (size_t frame = 0; frame < 512; ++frame) {
        cd_recovery[frame * 2] = 1800;
        cd_recovery[frame * 2 + 1] = -1800;
    }
    spu_cd_audio_push(cd_recovery, 512);

    double fill = 0.0, target = 0.0, correction = 0.0;
    uint64_t underruns_before = 0, underruns = 0, overflows = 0;
    uint64_t overflows_before = 0;
    int legacy = 1, host_rate = 0;
    cycle = 0;
    dc_audio_host_pump(device, 0, 0, 0);
    assert(SDL_GetQueuedAudioSize(device) == 0);
    assert(psx_audio_out_stats(&fill, &target, &underruns_before, &overflows,
                               &correction, &legacy, &host_rate) == 1);
    overflows_before = overflows;
    assert(legacy == 0 && host_rate == 44100);

    cycle = 768u * 50u;
    dc_audio_host_pump(device, 0, 0, 0);
    assert(psx_audio_out_stats(&fill, &target, &underruns, &overflows,
                               &correction, &legacy, &host_rate) == 1);
    assert(underruns == underruns_before + 1u);
    assert(overflows == overflows_before);
    assert(target == 60.0);

    /* The dump is written by the production queue() function, before SDL.
     * A zero-prefixed recovery would make the first PCM window silent. */
    FILE *file = fopen(path, "rb");
    assert(file);
    int16_t first[32 * 2];
    size_t got = fread(first, sizeof first, 1, file);
    fclose(file);
    assert(got == 1);
    int nonzero = 0;
    for (size_t i = 0; i < sizeof first / sizeof first[0]; ++i)
        if (first[i] != 0) { nonzero = 1; break; }
    assert(nonzero);
    assert(first[0] == 0 && first[1] == 0);

    /* A non-empty queue must not trigger the recovery again. */
    cycle += 768u;
    dc_audio_host_pump(device, 0, 0, 0);
    assert(psx_audio_out_stats(&fill, &target, &underruns, &overflows,
                               &correction, &legacy, &host_rate) == 1);
    assert(underruns == underruns_before + 1u);

    for (int i = 0; i < 6 && SDL_GetAudioDeviceStatus(device) == SDL_AUDIO_PAUSED; ++i) {
        generate_audio_frames(735);
        dc_audio_host_pump(device, 0, 0, 0);
    }
    assert_audio_status(device, SDL_AUDIO_PLAYING);

    /* A steady-state empty queue is a software queue observation, not a
     * recovery request. Real PCM is queued immediately and the device stays
     * playing. */
    file = fopen(path, "rb");
    assert(file);
    assert(fseek(file, 0, SEEK_END) == 0);
    long second_start = ftell(file);
    fclose(file);
    SDL_ClearQueuedAudio(device);
    assert(SDL_GetQueuedAudioSize(device) == 0);
    cycle += 768u * 50u;
    dc_audio_host_pump(device, 0, 0, 0);
    assert(psx_audio_out_stats(&fill, &target, &underruns, &overflows,
                               &correction, &legacy, &host_rate) == 1);
    assert(underruns == underruns_before + 2u);
    assert_audio_status(device, SDL_AUDIO_PLAYING);
    file = fopen(path, "rb");
    assert(file);
    assert(fseek(file, 0, SEEK_END) == 0);
    long bytes = ftell(file);
    assert(bytes > second_start + (long)sizeof first);
    assert(fseek(file, second_start, SEEK_SET) == 0);
    got = fread(first, sizeof first, 1, file);
    fclose(file);
    assert(got == 1);

    /* With no newly rendered guest frames, an empty queue must not invent PCM. */
    long before_no_frames = bytes;
    SDL_ClearQueuedAudio(device);
    dc_audio_host_pump(device, 0, 0, 0);
    assert_audio_status(device, SDL_AUDIO_PLAYING);
    file = fopen(path, "rb");
    assert(file);
    assert(fseek(file, 0, SEEK_END) == 0);
    assert(ftell(file) == before_no_frames);
    fclose(file);
    dc_audio_host_shutdown();
    set_audio_env("DC_AUDIO_WAV", "");
    remove(path);
}

static void test_recovery_buffer_and_pause_ownership(uint32_t device)
{
    char path[1024];
    char *base = SDL_GetBasePath();
    assert(base);
    int path_length = snprintf(path, sizeof path,
                               "%sdc_audio_host_recovery_contract.s16le", base);
    SDL_free(base);
    assert(path_length > 0 && (size_t)path_length < sizeof path);
    remove(path);

    dc_audio_host_shutdown();
    set_audio_env("DC_AUDIO_DRC", "1");
    set_audio_env("DC_AUDIO_DRC_MAX", "0.01");
    set_audio_env("DC_AUDIO_TARGET_MS", "60");
    set_audio_env("DC_AUDIO_AHEAD_MS", "30");
    set_audio_env("DC_MENU_AUDIO_FADEIN_MS", "0");
    set_audio_env("DC_AUDIO_WAV", path);
    setup_audio(device);
    SDL_PauseAudioDevice(device, 0);
    assert_audio_status(device, SDL_AUDIO_PLAYING);
    assert(SDL_GetQueuedAudioSize(device) == 0);

    generate_audio_frames(735);
    uint32_t size = spu_snapshot_core_bytes();
    uint32_t cd_size = spu_cd_snapshot_bytes();
    uint8_t *core = malloc(size), *ram = malloc(SPU_RAM_SIZE), *cd = malloc(cd_size);
    assert(core && ram && cd);
    spu_snapshot_core_write(core);
    memcpy(ram, spu_ram, SPU_RAM_SIZE);
    spu_cd_snapshot_write(cd);
    dc_audio_host_pump(device, 0, 0, 0);

    uint32_t queued_frames = SDL_GetQueuedAudioSize(device) / 4u;
    assert(queued_frames > 0 && queued_frames < 2646u);
    assert_guest_state_without_output(core, size, ram, cd, cd_size);
    assert_real_pcm_dump(path);
    assert_audio_status(device, SDL_AUDIO_PAUSED);

    int reached_prefill = 0;
    for (int i = 0; i < 4; ++i) {
        generate_audio_frames(735);
        dc_audio_host_pump(device, 0, 0, 0);
        queued_frames = SDL_GetQueuedAudioSize(device) / 4u;
        if (queued_frames < 2646u) {
            assert_audio_status(device, SDL_AUDIO_PAUSED);
        } else {
            reached_prefill = 1;
            assert_audio_status(device, SDL_AUDIO_PLAYING);
            break;
        }
    }
    assert(reached_prefill);
    free(core); free(ram); free(cd);

    /* An externally paused device must remain paused across every host path. */
    dc_audio_host_shutdown();
    setup_audio(device);
    SDL_PauseAudioDevice(device, 1);
    generate_audio_frames(735);
    dc_audio_host_pump(device, 0, 0, 0);
    assert_audio_status(device, SDL_AUDIO_PAUSED);
    dc_audio_host_reset(device);
    assert_audio_status(device, SDL_AUDIO_PAUSED);
    dc_audio_host_pump(device, 1, 0, 0);
    assert_audio_status(device, SDL_AUDIO_PAUSED);
    dc_audio_host_pump(device, 0, 1, 0);
    assert_audio_status(device, SDL_AUDIO_PAUSED);
    dc_audio_host_shutdown();
    assert_audio_status(device, SDL_AUDIO_PAUSED);

    /* Only a pause owned by the host may be released by lifecycle transitions. */
    start_host_owned_pause(device);
    dc_audio_host_reset(device);
    assert_audio_status(device, SDL_AUDIO_PLAYING);

    start_host_owned_pause(device);
    dc_audio_host_pump(device, 1, 0, 0);
    assert_audio_status(device, SDL_AUDIO_PLAYING);

    start_host_owned_pause(device);
    dc_audio_host_pump(device, 0, 1, 0);
    assert_audio_status(device, SDL_AUDIO_PLAYING);

    start_host_owned_pause(device);
    dc_audio_host_shutdown();
    assert_audio_status(device, SDL_AUDIO_PLAYING);

    set_audio_env("DC_AUDIO_WAV", "");
    remove(path);
}

typedef struct TestAudioClock {
    uint64_t now_ns;
    uint64_t step_ns;
    uint32_t calls;
} TestAudioClock;

static uint64_t test_audio_clock_now(void *opaque)
{
    TestAudioClock *clock = (TestAudioClock *)opaque;
    uint64_t now = clock->now_ns;
    clock->now_ns += clock->step_ns;
    clock->calls++;
    return now;
}

static void test_timing_disabled(uint32_t device)
{
    set_audio_env("DC_AUDIO_TIMING", "0");
    dc_audio_host_shutdown(); setup_audio(device);
    TestAudioClock clock = {0};
    dc_audio_host_test_set_clock(test_audio_clock_now, &clock);
    cycle = 768u * 50u; spu_catch_up();
    uint32_t size = spu_snapshot_core_bytes(), cd_size = spu_cd_snapshot_bytes();
    uint8_t *core = malloc(size), *ram = malloc(SPU_RAM_SIZE), *cd = malloc(cd_size);
    assert(core && ram && cd);
    spu_snapshot_core_write(core); memcpy(ram, spu_ram, SPU_RAM_SIZE);
    spu_cd_snapshot_write(cd);
    dc_audio_host_pump(device, 0, 0, 0);
    assert_guest_state_without_output(core, size, ram, cd, cd_size);
    DcAudioHostTimingSnapshot snapshot;
    assert(!dc_audio_host_timing_snapshot(&snapshot));
    assert(clock.calls == 0);
    dc_audio_host_test_set_clock(NULL, NULL);
    free(core); free(ram); free(cd);
}

static void test_timing_real_pcm_and_gap(uint32_t device)
{
    set_audio_env("DC_AUDIO_TIMING", "1");
    set_audio_env("DC_AUDIO_DRC", "1");
    set_audio_env("DC_AUDIO_DRC_MAX", "0.01");
    set_audio_env("DC_AUDIO_TARGET_MS", "180");
    dc_audio_host_shutdown(); setup_audio(device);
    TestAudioClock clock = {0, 1000, 0};
    dc_audio_host_test_set_clock(test_audio_clock_now, &clock);
    SDL_PauseAudioDevice(device, 0);
    cycle = 0;
    for (int i = 0; i < 4; ++i) {
        cycle += 768u * 2048u;
        dc_audio_host_pump(device, 0, 0, 0);
        if (i != 3) clock.now_ns += 1000000000u;
    }
    DcAudioHostTimingSnapshot snapshot;
    assert(dc_audio_host_timing_snapshot(&snapshot));
    assert(snapshot.enabled == 1);
    assert(snapshot.pump_calls == 4);
    assert(snapshot.pump_gap_count == 3);
    assert(snapshot.last_pump_gap_ns > 0);
    assert(snapshot.current_input_available_frames > 0);
    assert(snapshot.starvations == 1);
    assert(snapshot.recovery_starts == 1);
    assert(snapshot.recovery_ends == 1);
    assert(snapshot.catch_up_duration_total_ns > 0);
    assert(snapshot.submit_count > 0);
    assert(snapshot.submit_duration_total_ns > 0);
    assert(snapshot.pump_duration_total_ns > 0);
    assert(snapshot.event_count <= DC_AUDIO_TIMING_EVENT_CAPACITY);
    int found_recovery_end = 0;
    for (uint32_t i = 0; i < snapshot.event_count; ++i) {
        const DcAudioHostTimingEvent *event = &snapshot.events[i];
        if (event->kind != DC_AUDIO_TIMING_RECOVERY_END) continue;
        found_recovery_end = 1;
        assert(event->previous_pump_queued_frames > 0);
        assert(event->previous_submit_queued_frames > 0);
        assert(event->since_previous_pump_ns >
               (uint64_t)event->previous_pump_queued_frames * 1000000000u / 44100u);
        assert(event->since_previous_submit_ns > 0);
        assert(event->catch_up_duration_ns > 0);
        assert(event->queue_submit_duration_ns > 0);
        assert(event->pump_duration_ns > 0);
    }
    assert(found_recovery_end);
    clock.now_ns += 1000000000u;
    SDL_ClearQueuedAudio(device);
    cycle += 768u * 2048u;
    dc_audio_host_pump(device, 0, 0, 0);
    assert(dc_audio_host_timing_snapshot(&snapshot));
    assert(snapshot.starvations == 2);
    assert(snapshot.recovery_starts == 1);
    assert(snapshot.recovery_ends == 1);
    assert_audio_status(device, SDL_AUDIO_PLAYING);
    uint64_t newest_stall_sequence = 0;
    const DcAudioHostTimingEvent *newest_stall = NULL;
    for (uint32_t i = 0; i < snapshot.event_count; ++i) {
        const DcAudioHostTimingEvent *event = &snapshot.events[i];
        if (event->kind == DC_AUDIO_TIMING_STARVATION &&
            event->sequence > newest_stall_sequence) {
            newest_stall_sequence = event->sequence;
            newest_stall = event;
        }
    }
    assert(newest_stall);
    assert(newest_stall->device_status == SDL_AUDIO_PLAYING);
    assert(newest_stall->previous_pump_queued_frames > 0);
    assert(newest_stall->previous_submit_queued_frames > 0);
    assert(newest_stall->since_previous_pump_ns >= 1000000000u);
    assert(newest_stall->since_previous_submit_ns >= 1000000000u);
    dc_audio_host_test_set_clock(NULL, NULL);
    dc_audio_host_shutdown();
}

static void test_steady_queue_clear_continuity(uint32_t device)
{
    char *base = SDL_GetBasePath();
    assert(base);
    char control_path[1024], clear_path[1024];
    int control_length = snprintf(control_path, sizeof control_path,
                                  "%sdc_audio_host_steady_control.s16le", base);
    int clear_length = snprintf(clear_path, sizeof clear_path,
                                "%sdc_audio_host_steady_clear.s16le", base);
    SDL_free(base);
    assert(control_length > 0 && (size_t)control_length < sizeof control_path);
    assert(clear_length > 0 && (size_t)clear_length < sizeof clear_path);
    remove(control_path); remove(clear_path);

    set_audio_env("DC_AUDIO_TIMING", "1");
    set_audio_env("DC_AUDIO_DRC", "1");
    set_audio_env("DC_AUDIO_DRC_MAX", "0");
    set_audio_env("DC_AUDIO_TARGET_MS", "60");
    set_audio_env("DC_AUDIO_AHEAD_MS", "0");
    set_audio_env("DC_MENU_AUDIO_FADEIN_MS", "0");

    dc_audio_host_shutdown();
    set_audio_env("DC_AUDIO_WAV", control_path);
    setup_audio(device);
    SDL_PauseAudioDevice(device, 0);
    for (int i = 0; i < 8; ++i) {
        generate_audio_frames(735);
        dc_audio_host_pump(device, 0, 0, 0);
    }
    assert_audio_status(device, SDL_AUDIO_PLAYING);
    generate_audio_frames(735);
    dc_audio_host_pump(device, 0, 0, 0);
    dc_audio_host_shutdown();

    set_audio_env("DC_AUDIO_WAV", clear_path);
    setup_audio(device);
    SDL_PauseAudioDevice(device, 0);
    for (int i = 0; i < 8; ++i) {
        generate_audio_frames(735);
        dc_audio_host_pump(device, 0, 0, 0);
    }
    assert_audio_status(device, SDL_AUDIO_PLAYING);
    DcAudioHostTimingSnapshot before_clear;
    assert(dc_audio_host_timing_snapshot(&before_clear));
    assert(before_clear.recovery_starts > 0);
    SDL_ClearQueuedAudio(device);
    generate_audio_frames(735);
    dc_audio_host_pump(device, 0, 0, 0);
    assert_audio_status(device, SDL_AUDIO_PLAYING);
    DcAudioHostTimingSnapshot after_clear;
    assert(dc_audio_host_timing_snapshot(&after_clear));
    assert(after_clear.starvations == before_clear.starvations + 1u);
    assert(after_clear.recovery_starts == before_clear.recovery_starts);
    assert(after_clear.recovery_ends == before_clear.recovery_ends);
    assert(after_clear.current_input_available_frames > 0);
    assert(after_clear.current_delivered_frames > 0);
    dc_audio_host_shutdown();

    assert_files_equal(control_path, clear_path);
    set_audio_env("DC_AUDIO_WAV", "");
    set_audio_env("DC_AUDIO_DRC_MAX", "0.01");
    remove(control_path); remove(clear_path);
}

static void test_timing_exclusions(uint32_t device)
{
    set_audio_env("DC_AUDIO_TIMING", "1");
    set_audio_env("DC_AUDIO_DRC", "1");
    set_audio_env("DC_AUDIO_DRC_MAX", "0.01");
    set_audio_env("DC_AUDIO_TARGET_MS", "60");
    dc_audio_host_shutdown(); setup_audio(device);
    TestAudioClock clock = {0, 0, 0};
    dc_audio_host_test_set_clock(test_audio_clock_now, &clock);
    SDL_PauseAudioDevice(device, 1);
    cycle = 768u * 50u; dc_audio_host_pump(device, 0, 0, 0);
    DcAudioHostTimingSnapshot before_resume;
    assert(dc_audio_host_timing_snapshot(&before_resume));
    assert(before_resume.starvations == 0);
    assert(before_resume.recovery_starts == 0);
    clock.now_ns = 100000000u;
    SDL_PauseAudioDevice(device, 0);
    cycle += 768u * 50u;
    dc_audio_host_pump(device, 0, 0, 0);
    DcAudioHostTimingSnapshot after_resume;
    assert(dc_audio_host_timing_snapshot(&after_resume));
    assert(after_resume.pump_gap_count == before_resume.pump_gap_count);
    assert(after_resume.event_count == 0 ||
           after_resume.events[0].since_previous_pump_ns == 0);

    dc_audio_host_shutdown(); setup_audio(device);
    clock.now_ns = 0;
    clock.calls = 0;
    SDL_PauseAudioDevice(device, 1);
    cycle = 768u * 50u; dc_audio_host_pump(device, 0, 0, 0);
    DcAudioHostTimingSnapshot before_hold;
    assert(dc_audio_host_timing_snapshot(&before_hold));
    assert(before_hold.starvations == 0);
    assert(before_hold.recovery_starts == 0);
    clock.now_ns = 100000000u;
    dc_audio_host_pump(device, 1, 0, 0);
    clock.now_ns = 200000000u;
    dc_audio_host_pump(device, 0, 0, 0);
    DcAudioHostTimingSnapshot after_hold;
    assert(dc_audio_host_timing_snapshot(&after_hold));
    assert(after_hold.starvations == 0);
    assert(after_hold.recovery_starts == 0);
    assert(after_hold.pump_gap_count == 0);
    assert(after_hold.event_sequence == before_hold.event_sequence);
    dc_audio_host_test_set_clock(NULL, NULL);
    dc_audio_host_shutdown();
}

int main(void)
{
    test_process_init();
    assert(SDL_Init(SDL_INIT_AUDIO) == 0);
    SDL_AudioSpec want = {0}, have = {0};
    want.freq = 44100; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 512;
    SDL_AudioDeviceID device = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    assert(device && have.freq == 44100 && have.channels == 2 && have.format == AUDIO_S16SYS);
    test_host_independent_guest_time(device); test_hold_and_reset(device); test_output_backlog(device);
    test_empty_queue_recovery(device);
    test_recovery_buffer_and_pause_ownership(device);
    test_timing_disabled(device);
    test_timing_real_pcm_and_gap(device);
    test_steady_queue_clear_continuity(device);
    test_timing_exclusions(device);
    dc_audio_host_shutdown(); SDL_CloseAudioDevice(device); SDL_Quit(); dc_spu_pages_reset();
    puts("PASS: real SDL output pump preserves guest SPU/RAM/CD state across host conditions");
    return 0;
}
