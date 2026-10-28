#ifndef DC_CREATURE_BLOOD_H
#define DC_CREATURE_BLOOD_H
#include "cpu_state.h"
#ifdef __cplusplus
extern "C" {
#endif
int dc_creature_blood_defer(CPUState *cpu, uint32_t pc, uint32_t instruction, uint32_t address, uint16_t value);
int dc_creature_blood_flush(CPUState *cpu, uint32_t next_pc);
int dc_creature_blood_resume(CPUState *cpu, uint32_t pc);
int dc_creature_blood_pending(CPUState *cpu, uint32_t pc);
#ifdef __cplusplus
}
#endif
#endif
