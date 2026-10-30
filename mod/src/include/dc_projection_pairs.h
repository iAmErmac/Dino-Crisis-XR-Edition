#ifndef DC_PROJECTION_PAIRS_H
#define DC_PROJECTION_PAIRS_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
uint32_t dc_pair_projection_note(const int16_t rotation[9], const int32_t translation[3],
    const int16_t vertex[3], int reaim, const float delta_r[9], const float delta_t[3]);
void dc_pair_projection_tag(uint32_t tag, float x, float y);
void dc_pair_projection_reset(void);
extern int g_dc_pair_track;
extern uint32_t g_dc_pair_last;
#ifdef __cplusplus
}
#endif
#endif
