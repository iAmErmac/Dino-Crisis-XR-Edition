#ifndef DC_MEMORY_SNAPSHOT_H
#define DC_MEMORY_SNAPSHOT_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
size_t dc_memory_snapshot_bytes(void);
int dc_memory_snapshot_write(uint8_t *data, size_t size);
int dc_memory_snapshot_validate(const uint8_t *data, size_t size);
int dc_memory_snapshot_read(const uint8_t *data, size_t size);
#ifdef __cplusplus
}
#endif
#endif
