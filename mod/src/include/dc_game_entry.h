#ifndef DC_GAME_ENTRY_H
#define DC_GAME_ENTRY_H

#include "cpu_state.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { DC_GAME_ENTRY_RING_CAP = 32, DC_GAME_ENTRY_RING2_CAP = 4096 };

extern volatile uint32_t g_dc_fn_ring[DC_GAME_ENTRY_RING_CAP];
extern volatile uint32_t g_dc_fn_pos;
extern volatile uint32_t g_fn_ring2[DC_GAME_ENTRY_RING2_CAP];
extern volatile uint32_t g_fn_ring2_pos;

void dc_game_entry(CPUState *cpu, uint32_t func_addr);

#ifdef __cplusplus
}
#endif

#endif
