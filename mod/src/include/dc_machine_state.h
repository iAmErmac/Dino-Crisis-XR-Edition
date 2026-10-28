#ifndef DC_MACHINE_STATE_H
#define DC_MACHINE_STATE_H
#include <stdint.h>
#include "cpu_state.h"
#ifdef __cplusplus
extern "C" {
#endif
int dc_machine_state_capture(uint8_t **data, uint32_t *size);
int dc_machine_state_validate(const uint8_t *data, uint32_t size);
int dc_machine_state_restore(const uint8_t *data, uint32_t size, CPUState *cpu);
#ifdef __cplusplus
}
#endif
#endif
