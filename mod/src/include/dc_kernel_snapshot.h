#ifndef DC_KERNEL_SNAPSHOT_H
#define DC_KERNEL_SNAPSHOT_H
#include <stddef.h>
#include <stdint.h>
#include "cpu_state.h"
#ifdef __cplusplus
extern "C" {
#endif
size_t dc_kernel_snapshot_bytes(void);
int dc_kernel_snapshot_write(uint8_t *data, size_t size);
int dc_kernel_snapshot_validate(const uint8_t *data, size_t size);
int dc_kernel_snapshot_read(const uint8_t *data, size_t size, CPUState *cpu);
#ifdef __cplusplus
}
#endif
#endif
