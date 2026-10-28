#ifndef DC_DRAW_POSE_H
#define DC_DRAW_POSE_H
#include <stdint.h>
#include "cpu_state.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct DcDrawPose {
    uint32_t applied, papplied, gapplied, rapplied, t0applied, ladder_applied;
    int16_t saved[9], wrote[9], psaved[9], pwrote[9];
    int16_t gsaved[2][3][16], gwrote[2][3][16];
    int16_t rsaved[3], rwrote[3], t0saved[16], t0wrote[16];
    int16_t ladder_saved[3], ladder_wrote[3];
} DcDrawPose;

void dc_ladder_draw_capture(DcDrawPose *pose);
void dc_ladder_draw_restore(CPUState *cpu, const DcDrawPose *pose);
#ifdef __cplusplus
}
#endif
#endif
