#include "dc_spu_pages.h"
#include "pst_wire.h"
#include <stdlib.h>
#include <string.h>

extern void psx_fatal_halt(const char *reason);
static uint8_t *s_pages[DC_SPU_PAGE_COUNT];
static uint32_t s_capture;
static struct { uint32_t page, address, selected; } s_voices[DC_SPU_PAGE_VOICES];
enum { PAGE_WIRE_VERSION = 1, PAGE_WIRE_HEADER = DC_SPU_PAGE_STATE_BYTES };
static _Noreturn void fail(const char *reason) { psx_fatal_halt(reason); abort(); }

uint8_t *spu_xpage(int page)
{
    if (page < 1 || page > DC_SPU_PAGE_COUNT) return NULL;
    uint8_t **storage = &s_pages[page - 1];
    if (!*storage) {
        *storage = calloc(1, DC_SPU_PAGE_BYTES);
        if (!*storage) fail("SPU: extended sound page allocation failed");
    }
    return *storage;
}
void spu_xpage_capture(int page)
{
    s_capture = page >= 1 && page <= DC_SPU_PAGE_COUNT ? (uint32_t)page : 0;
}
void dc_spu_page_start(unsigned voice, uint16_t start)
{
    if (voice >= DC_SPU_PAGE_VOICES) fail("SPU: invalid hardware voice binding");
    s_voices[voice].page = s_capture;
    s_voices[voice].address = ((uint32_t)(start & ~1u) << 3) & (DC_SPU_PAGE_BYTES - 1u);
}
void dc_spu_page_key_on(unsigned voice, uint32_t address)
{
    if (voice >= DC_SPU_PAGE_VOICES) fail("SPU: invalid hardware voice key-on");
    uint32_t page = s_voices[voice].page;
    s_voices[voice].selected = page && s_voices[voice].address == address && s_pages[page - 1] ? page : 0;
}
const uint8_t *dc_spu_page_memory(unsigned voice)
{
    if (voice >= DC_SPU_PAGE_VOICES) return NULL;
    uint32_t page = s_voices[voice].selected;
    return page ? s_pages[page - 1] : NULL;
}
void dc_spu_pages_reset(void)
{
    for (unsigned i = 0; i < DC_SPU_PAGE_COUNT; ++i) {
        free(s_pages[i]); s_pages[i] = NULL;
    }
    s_capture = 0;
    memset(s_voices, 0, sizeof s_voices);
}
uint32_t dc_spu_pages_snapshot_bytes(void)
{
    uint32_t size = PAGE_WIRE_HEADER;
    for (unsigned i = 0; i < DC_SPU_PAGE_COUNT; ++i)
        if (s_pages[i]) size += DC_SPU_PAGE_BYTES;
    return size;
}
void dc_spu_pages_snapshot_write(uint8_t *data)
{
    if (!data) fail("SPU: missing sound page snapshot buffer");
    uint32_t mask = 0;
    for (unsigned i = 0; i < DC_SPU_PAGE_COUNT; ++i) if (s_pages[i]) mask |= 1u << i;
    PstW out; pst_w_init(&out, data, dc_spu_pages_snapshot_bytes());
    pst_w_u32(&out, PAGE_WIRE_VERSION); pst_w_u32(&out, mask); pst_w_u32(&out, s_capture);
    for (unsigned i = 0; i < DC_SPU_PAGE_VOICES; ++i) {
        pst_w_u32(&out, s_voices[i].page); pst_w_u32(&out, s_voices[i].address);
        pst_w_u32(&out, s_voices[i].selected);
    }
    for (unsigned i = 0; i < DC_SPU_PAGE_COUNT; ++i)
        if (s_pages[i]) pst_w_bytes(&out, s_pages[i], DC_SPU_PAGE_BYTES);
}
int dc_spu_pages_snapshot_validate(const uint8_t *data, uint32_t size)
{
    if (!data || size < PAGE_WIRE_HEADER) return 0;
    PstR in; uint32_t version, mask, capture;
    pst_r_init(&in, data, size);
    if (!pst_r_u32(&in, &version) || version != PAGE_WIRE_VERSION ||
        !pst_r_u32(&in, &mask) || (mask & ~0xFFu) ||
        !pst_r_u32(&in, &capture) || capture > DC_SPU_PAGE_COUNT) return 0;
    for (unsigned i = 0; i < DC_SPU_PAGE_VOICES; ++i) {
        uint32_t page, address, selected;
        if (!pst_r_u32(&in, &page) || page > DC_SPU_PAGE_COUNT ||
            !pst_r_u32(&in, &address) || address >= DC_SPU_PAGE_BYTES || (address & 15u) ||
            !pst_r_u32(&in, &selected) || selected > DC_SPU_PAGE_COUNT ||
            (selected && !(mask & (1u << (selected - 1))))) return 0;
    }
    uint32_t expected = PAGE_WIRE_HEADER;
    for (unsigned i = 0; i < DC_SPU_PAGE_COUNT; ++i) if (mask & (1u << i)) expected += DC_SPU_PAGE_BYTES;
    return size == expected;
}
int dc_spu_pages_snapshot_read(const uint8_t *data, uint32_t size)
{
    if (!dc_spu_pages_snapshot_validate(data, size)) return 0;
    PstR in; uint32_t version, mask, capture;
    uint8_t *pages[DC_SPU_PAGE_COUNT] = {0};
    pst_r_init(&in, data, size);
    pst_r_u32(&in, &version); pst_r_u32(&in, &mask); pst_r_u32(&in, &capture);
    const uint8_t *voices = in.p;
    in.p += DC_SPU_PAGE_VOICES * 12;
    for (unsigned i = 0; i < DC_SPU_PAGE_COUNT; ++i) if (mask & (1u << i)) {
        pages[i] = malloc(DC_SPU_PAGE_BYTES);
        if (!pages[i]) fail("SPU: sound page restore allocation failed");
        pst_r_bytes(&in, pages[i], DC_SPU_PAGE_BYTES);
    }
    dc_spu_pages_reset();
    memcpy(s_pages, pages, sizeof pages); s_capture = capture;
    pst_r_init(&in, voices, DC_SPU_PAGE_VOICES * 12);
    for (unsigned i = 0; i < DC_SPU_PAGE_VOICES; ++i) {
        pst_r_u32(&in, &s_voices[i].page); pst_r_u32(&in, &s_voices[i].address);
        pst_r_u32(&in, &s_voices[i].selected);
    }
    return 1;
}
