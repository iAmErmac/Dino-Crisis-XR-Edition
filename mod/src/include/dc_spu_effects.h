#ifndef DC_SPU_EFFECTS_H
#define DC_SPU_EFFECTS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { DC_SPU_EFFECT_VOICE_COUNT = 24 };

void dc_spu_effects_reset(void);
void dc_spu_effect_key_on(unsigned voice, const uint8_t *bytes, uint32_t size,
    uint32_t address);
int dc_spu_effect_next(unsigned voice, uint16_t pitch, int16_t *raw, int *ended);
void dc_spu_effect_retire(unsigned voice);
int dc_spu_effect_pcm_busy(const int16_t *pcm);

#ifdef __cplusplus
}
#endif

#endif
