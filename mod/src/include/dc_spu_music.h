#ifndef DC_SPU_MUSIC_H
#define DC_SPU_MUSIC_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void dc_spu_music_reset(void);
void dc_spu_music_poll(void);
void dc_spu_music_key_on(unsigned voice);
int dc_spu_music_muted(unsigned voice);
void dc_spu_music_mix(int32_t *left, int32_t *right);
int dc_spu_music_pcm_busy(const int16_t *pcm);
void dc_spu_music_changed(void);
#ifdef __cplusplus
}
#endif
#endif
