#ifndef DC_AUDIO_OUTPUT_H
#define DC_AUDIO_OUTPUT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DcAudioOutput {
    double position;
    double average_queue;
    int16_t previous[2];
    int16_t last_output[2];
    uint32_t fade_remaining;
    uint32_t fade_total;
    int gap;
    double rate_adjust;
} DcAudioOutput;

typedef struct DcAudioOutputConfig {
    int drc;
    uint32_t target_frames;
    double max_adjust;
} DcAudioOutputConfig;

void dc_audio_output_reset(DcAudioOutput *state);
void dc_audio_output_begin_fade(DcAudioOutput *state, uint32_t frames);
int dc_audio_output_process(DcAudioOutput *state,
    const DcAudioOutputConfig *config,
    const int16_t *input,
    uint32_t frames,
    uint32_t queued_frames,
    int deliver,
    int16_t *output,
    uint32_t output_capacity);

#ifdef __cplusplus
}
#endif

#endif
