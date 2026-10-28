#ifndef DC_AUDIO_HOST_H
#define DC_AUDIO_HOST_H
#include <stdint.h>

#define DC_AUDIO_TIMING_EVENT_CAPACITY 16u

enum {
    DC_AUDIO_TIMING_NO_PCM_EMPTY = 1,
    DC_AUDIO_TIMING_STARVATION = 2,
    DC_AUDIO_TIMING_RECOVERY_START = 3,
    DC_AUDIO_TIMING_RECOVERY_END = 4
};

typedef struct DcAudioHostTimingEvent {
    uint64_t sequence;
    uint32_t kind;
    uint32_t device;
    uint64_t host_ns;
    uint64_t since_previous_pump_ns;
    uint64_t since_previous_submit_ns;
    uint64_t pump_duration_ns;
    uint64_t catch_up_duration_ns;
    uint64_t queue_submit_duration_ns;
    uint32_t previous_pump_queued_frames;
    uint32_t previous_submit_queued_frames;
    uint32_t queued_before_frames;
    uint32_t queued_after_frames;
    uint32_t input_available_frames;
    uint32_t input_popped_frames;
    uint32_t delivered_frames;
    uint32_t device_status;
    uint32_t recovery_owned;
} DcAudioHostTimingEvent;

typedef struct DcAudioHostTimingSnapshot {
    int enabled;
    uint32_t event_count;
    uint64_t event_sequence;
    uint64_t pump_calls;
    uint64_t pump_gap_count;
    uint64_t pump_gap_total_ns;
    uint64_t pump_gap_max_ns;
    uint64_t last_pump_gap_ns;
    uint64_t pump_duration_total_ns;
    uint64_t pump_duration_max_ns;
    uint64_t last_pump_duration_ns;
    uint64_t catch_up_duration_total_ns;
    uint64_t catch_up_duration_max_ns;
    uint64_t last_catch_up_duration_ns;
    uint64_t submit_count;
    uint64_t submit_duration_total_ns;
    uint64_t submit_duration_max_ns;
    uint64_t last_submit_duration_ns;
    uint64_t last_submit_gap_ns;
    uint64_t no_pcm_episodes;
    uint64_t starvations;
    uint64_t recovery_starts;
    uint64_t recovery_ends;
    uint32_t current_queued_frames;
    uint32_t current_input_available_frames;
    uint32_t current_input_popped_frames;
    uint32_t current_delivered_frames;
    uint32_t current_device_status;
    uint32_t recovery_owned;
    DcAudioHostTimingEvent events[DC_AUDIO_TIMING_EVENT_CAPACITY];
} DcAudioHostTimingSnapshot;

#ifdef __cplusplus
extern "C" {
#endif
void dc_audio_host_pump(uint32_t device, int held, int burst, int xa_active);
void dc_audio_host_reset(uint32_t device);
void dc_audio_host_shutdown(void);
int dc_audio_host_timing_snapshot(DcAudioHostTimingSnapshot *snapshot);
#if defined(DC_AUDIO_HOST_TEST)
typedef uint64_t (*DcAudioHostTestClock)(void *opaque);
void dc_audio_host_test_set_clock(DcAudioHostTestClock clock, void *opaque);
#endif
#ifdef __cplusplus
}
#endif
#endif
