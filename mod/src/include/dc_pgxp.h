#ifndef DC_PGXP_H
#define DC_PGXP_H

#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

void dc_pgxp_project_float(int vertex, float x, float y, float depth, uint32_t packed, int valid);

/* Observe completed guest operations without repeating their memory effects. */
void dc_pgxp_load(uint32_t instruction, uint32_t address, uint32_t value);
void dc_pgxp_store(uint32_t instruction, uint32_t address, uint32_t value);
void dc_pgxp_alu(uint32_t instruction, uint32_t result);
void dc_pgxp_cop2(uint32_t instruction, uint32_t value, uint32_t address);
void dc_pgxp_forget_register(uint32_t reg);
void dc_pgxp_project(int vertex, int32_t x16, int32_t y16,
                     uint32_t depth, uint32_t packed, int valid);

#ifdef __cplusplus
}
#endif
#endif
