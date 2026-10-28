#ifndef DC_DMA_POLICY_H
#define DC_DMA_POLICY_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define DC_DMA_POLICY_WIRE_BYTES 12u
void dc_dma_policy_begin(void);
void dc_dma_policy_node(uint32_t address);
void dc_dma_policy_finish(void);
void dc_dma_policy_cancel(void);
void dc_dma_policy_otc(uint32_t address);
int dc_dma_policy_write(uint8_t *data, size_t size, int active);
int dc_dma_policy_validate(const uint8_t *data, size_t size, int active);
int dc_dma_policy_read(const uint8_t *data, size_t size, int active);
#ifdef __cplusplus
}
#endif
#endif
