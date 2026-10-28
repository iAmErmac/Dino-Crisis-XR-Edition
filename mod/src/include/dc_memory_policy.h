#ifndef DC_MEMORY_POLICY_H
#define DC_MEMORY_POLICY_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void dc_memory_policy_bind(void);
void dc_memory_preload(void);
uint32_t psx_ext_base(void);
uint32_t psx_ext_size(void);
const uint8_t *memory_mod_ptr(uint32_t phys, uint32_t width);
uint32_t memory_mod_word(uint32_t phys);
int memory_fk_code(uint32_t phys);
uint32_t dc_arena_base(void);
uint32_t dc_node_alloc_extra(void);
int dc_memory_extra_span(uint32_t phys, uint32_t width);
uint8_t *dc_memory_extra_ptr(uint32_t phys, uint32_t width);
int dc_memory_policy_write(uint32_t phys, uint32_t *value, uint32_t width);
void dc_nots_readiness(int *ready, uint64_t *age);
/* Packet storage takes precedence over console RAM mirrors. */
static inline int dc_memory_extra_address(uint32_t phys) {
    return phys >= 0x00400000u && phys < 0x1F000000u &&
           dc_memory_extra_span(phys, 1);
}
#ifdef __cplusplus
}
#endif
#endif
