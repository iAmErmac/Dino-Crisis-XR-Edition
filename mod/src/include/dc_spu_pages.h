#ifndef DC_SPU_PAGES_H
#define DC_SPU_PAGES_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
enum { DC_SPU_PAGE_BYTES = 0x80000u, DC_SPU_PAGE_COUNT = 8, DC_SPU_PAGE_VOICES = 24,
       DC_SPU_PAGE_STATE_BYTES = 12 + DC_SPU_PAGE_VOICES * 12 };
uint8_t *spu_xpage(int page);
void spu_xpage_capture(int page);
void dc_spu_page_start(unsigned voice, uint16_t start);
void dc_spu_page_key_on(unsigned voice, uint32_t address);
const uint8_t *dc_spu_page_memory(unsigned voice);
void dc_spu_pages_reset(void);
uint32_t dc_spu_pages_snapshot_bytes(void);
void dc_spu_pages_snapshot_write(uint8_t *data);
int dc_spu_pages_snapshot_validate(const uint8_t *data, uint32_t size);
int dc_spu_pages_snapshot_read(const uint8_t *data, uint32_t size);
#ifdef __cplusplus
}
#endif
#endif
