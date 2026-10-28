#ifndef DC_BACKGROUND_H
#define DC_BACKGROUND_H

#include "cpu_state.h"

#ifdef __cplusplus
extern "C" {
#endif

int dc_background_enter(CPUState *cpu, uint32_t pc);
int dc_background_pending(CPUState *cpu, uint32_t pc);
void dc_background_resume(CPUState *cpu, uint32_t pc);

#ifdef __cplusplus
}
#endif
#endif
