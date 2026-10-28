#ifndef DC_FK_POLICY_H
#define DC_FK_POLICY_H
#include "cpu_state.h"
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
uint32_t dc_fk_bank(uint32_t pc);
uint32_t dc_fk_address(uint32_t pc, uint32_t address);
uint32_t dc_fk_pointer(uint32_t pc, uint32_t value);
void dc_fk_leave(CPUState *cpu, uint32_t pc, uint32_t target);
uint32_t dc_fk_load_word(uint32_t pc, uint32_t address, uint32_t value);
uint32_t dc_fk_store_word(uint32_t pc, uint32_t address, uint32_t value);
uint32_t dc_fk_special(uint32_t player, uint32_t offset, uint32_t current);
void dc_fk_gsv_reset(void);
void dc_fk_room_tick(int room);
size_t dc_fk_snapshot_bytes(void);
int dc_fk_snapshot_write(uint8_t *data, size_t size);
int dc_fk_snapshot_validate(const uint8_t *data, size_t size);
int dc_fk_snapshot_read(const uint8_t *data, size_t size);
#ifdef __cplusplus
}
#endif
#endif
