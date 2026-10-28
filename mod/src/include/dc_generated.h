#ifndef DC_GENERATED_H
#define DC_GENERATED_H

#include "cpu_state.h"
#include "dc_call_state.h"
#ifdef __cplusplus
extern "C" {
#endif

void gte_execute_pc(CPUState *cpu, uint32_t command, uint32_t pc);
extern uint32_t g_gte_site_pc;
void dc_geomcam_probe(CPUState *cpu);
void pt_dc_proj(CPUState *cpu, uint32_t model, uint32_t pool);
void pt_dc_prim(CPUState *cpu, uint32_t primitive, uint32_t packet, int vertices);
void pt_dc_poolprim(CPUState *cpu, uint32_t primitive, uint32_t pool,
                    uint32_t packet, int vertices);
int dc_rej(int kind);
int dc_room_taken(uint32_t data);

#ifdef __cplusplus
}
#endif
#endif
