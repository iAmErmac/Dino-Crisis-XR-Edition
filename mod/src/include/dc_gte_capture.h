#ifndef DC_GTE_CAPTURE_H
#define DC_GTE_CAPTURE_H
#include <stdint.h>
#include "cpu_state.h"
#ifdef __cplusplus
extern "C" {
#endif
void dc_gte_capture_projection(const int16_t rotation[9], const int32_t translation[3],
    uint32_t packed, float x, float y, float depth, int valid);
void dc_gte_capture_reset(void);
void dc_gte_light_note(const CPUState *cpu);
void dc_gte_light_reset(void);
int dc_gte_light_get(float direction[3], float color[3]);
int dc_gte_light_snap(int index, float light[9], float color[9], float rotation[9], float translation[3], long *hits);
int pt_current_matrix_epoch(void);
void pt_gte_read_matrix(CPUState *cpu, float rotation[9], float translation[3]);
extern float g_cam_R[9], g_cam_T[3];
extern int g_cam_have;
#ifdef __cplusplus
}
#endif
#endif
