#ifndef DC_SPU_POLICY_H
#define DC_SPU_POLICY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    DC_SPU_POLICY_VOICES = 24,
    DC_SPU_POLICY_RAM_BYTES = 0x80000u,
    DC_SPU_POLICY_STATE_BYTES = 104
};

void dc_spu_policy_reset(void);
void dc_spu_guard_key_on(unsigned voice, const uint8_t *native_ram,
    uint32_t address);
uint8_t dc_spu_guard_flags(unsigned voice, uint32_t address, uint8_t flags);
void dc_spu_cd_delay_reset(void);
void dc_spu_cd_delay_push(int was_empty);
int dc_spu_cd_delay_tick(int queued);
void dc_spu_policy_snapshot_write(uint8_t *data);
int dc_spu_policy_snapshot_validate(const uint8_t *data, uint32_t size);
int dc_spu_policy_snapshot_read(const uint8_t *data, uint32_t size);

#ifdef __cplusplus
}
#endif

#endif
