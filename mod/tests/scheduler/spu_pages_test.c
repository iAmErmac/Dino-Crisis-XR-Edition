#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "dc_spu_pages.h"
#include "test_process.h"

enum {
    WIRE_HEADER_BYTES = 300u,
    WIRE_VERSION_OFFSET = 0u,
    WIRE_MASK_OFFSET = 4u,
    WIRE_CAPTURE_OFFSET = 8u,
    WIRE_VOICE0_OFFSET = 12u,
    WIRE_VOICE_RECORD_BYTES = 12u,
    PAGE_PREFIX_BYTES = 32u
};

void psx_fatal_halt(const char *reason)
{
    (void)reason;
    abort();
}

static uint32_t normalized_start(uint16_t start)
{
    return ((uint32_t)(start & ~1u) << 3) &
           (DC_SPU_PAGE_BYTES - 1u);
}

static void fill_adpcm(uint8_t *page, uint8_t seed)
{
    for (uint32_t i = 0; i < PAGE_PREFIX_BYTES; ++i)
        page[i] = (uint8_t)(seed + i * 3u);
}

static void assert_adpcm(const uint8_t *page, uint8_t seed)
{
    for (uint32_t i = 0; i < PAGE_PREFIX_BYTES; ++i)
        assert(page[i] == (uint8_t)(seed + i * 3u));
}

static void assert_snapshot_unchanged(const uint8_t *expected, uint32_t size)
{
    uint8_t *actual = (uint8_t *)malloc(size);
    assert(actual);
    dc_spu_pages_snapshot_write(actual);
    assert(memcmp(actual, expected, size) == 0);
    free(actual);
}

static void reject_without_mutation(uint8_t *candidate, uint32_t size,
                                    const uint8_t *expected,
                                    uint32_t expected_size)
{
    assert(!dc_spu_pages_snapshot_validate(candidate, size));
    assert(!dc_spu_pages_snapshot_read(candidate, size));
    assert_snapshot_unchanged(expected, expected_size);
}

static void test_page_bounds_and_separation(void)
{
    uint8_t *page1;
    uint8_t *page2;

    dc_spu_pages_reset();
    assert(spu_xpage(0) == NULL);
    assert(spu_xpage(-1) == NULL);
    assert(spu_xpage(DC_SPU_PAGE_COUNT + 1) == NULL);

    page1 = spu_xpage(1);
    page2 = spu_xpage(2);
    assert(page1 && page2 && page1 != page2);
    fill_adpcm(page1, 0x11u);
    fill_adpcm(page2, 0x91u);
    assert_adpcm(page1, 0x11u);
    assert_adpcm(page2, 0x91u);
    assert(page1[0] != page2[0]);
}

static void test_capture_binding_and_fallback(void)
{
    const uint16_t odd_start = 0x0123u;
    const uint32_t address = normalized_start(odd_start);
    uint8_t *page1;
    uint8_t *page3;
    uint8_t *page8;

    dc_spu_pages_reset();
    page1 = spu_xpage(1);
    assert(page1);

    spu_xpage_capture(1);
    dc_spu_page_start(0, odd_start);
    dc_spu_page_key_on(0, address);
    assert(dc_spu_page_memory(0) == page1);

    /* Capture changes do not disturb a voice that is already playing. */
    spu_xpage_capture(2);
    assert(dc_spu_page_memory(0) == page1);

    /* A start written without a captured page removes the prior binding. */
    spu_xpage_capture(0);
    dc_spu_page_start(0, odd_start);
    assert(dc_spu_page_memory(0) == page1);
    dc_spu_page_key_on(0, address);
    assert(dc_spu_page_memory(0) == NULL);

    /* Invalid capture values clear the pending page for later starts. */
    spu_xpage_capture(DC_SPU_PAGE_COUNT + 1);
    dc_spu_page_start(1, odd_start);
    dc_spu_page_key_on(1, address);
    assert(dc_spu_page_memory(1) == NULL);

    /* A valid page number does not bind until its storage is allocated. */
    spu_xpage_capture(3);
    dc_spu_page_start(1, odd_start);
    dc_spu_page_key_on(1, address);
    assert(dc_spu_page_memory(1) == NULL);
    page3 = spu_xpage(3);
    assert(page3);
    dc_spu_page_key_on(1, address);
    assert(dc_spu_page_memory(1) == page3);
    dc_spu_page_key_on(1, address + 16u);
    assert(dc_spu_page_memory(1) == NULL);

    /* The last hardware voice is valid; the first private slot is not. */
    page8 = spu_xpage(8);
    assert(page8);
    spu_xpage_capture(8);
    dc_spu_page_start(DC_SPU_PAGE_VOICES - 1u, odd_start);
    dc_spu_page_key_on(DC_SPU_PAGE_VOICES - 1u, address);
    assert(dc_spu_page_memory(DC_SPU_PAGE_VOICES - 1u) == page8);
    assert(dc_spu_page_memory(DC_SPU_PAGE_VOICES) == NULL);
}

static void test_snapshot_round_trip_and_rejection(void)
{
    const uint16_t start0 = 0x0041u;
    const uint16_t start1 = 0x00A3u;
    const uint32_t address0 = normalized_start(start0);
    const uint32_t address1 = normalized_start(start1);
    uint8_t *page1;
    uint8_t *page3;
    uint8_t *snapshot;
    uint8_t *candidate;
    uint32_t size;

    dc_spu_pages_reset();
    page1 = spu_xpage(1);
    page3 = spu_xpage(3);
    assert(page1 && page3 && page1 != page3);
    fill_adpcm(page1, 0x21u);
    fill_adpcm(page3, 0x71u);

    spu_xpage_capture(3);
    dc_spu_page_start(0, start0);
    dc_spu_page_key_on(0, address0);
    assert(dc_spu_page_memory(0) == page3);
    spu_xpage_capture(1);
    dc_spu_page_start(DC_SPU_PAGE_VOICES - 1u, start1);
    dc_spu_page_key_on(DC_SPU_PAGE_VOICES - 1u, address1);
    assert(dc_spu_page_memory(DC_SPU_PAGE_VOICES - 1u) == page1);

    size = dc_spu_pages_snapshot_bytes();
    assert(size == WIRE_HEADER_BYTES + 2u * DC_SPU_PAGE_BYTES);
    snapshot = (uint8_t *)malloc(size);
    candidate = (uint8_t *)malloc(size);
    assert(snapshot && candidate);
    dc_spu_pages_snapshot_write(snapshot);
    assert(dc_spu_pages_snapshot_validate(snapshot, size));

    /* Every rejected input must leave the live pages and voice state intact. */
    assert(size > 1u);
    reject_without_mutation(snapshot, size - 1u, snapshot, size);

    memcpy(candidate, snapshot, size);
    candidate[WIRE_VERSION_OFFSET] = 0xFFu;
    reject_without_mutation(candidate, size, snapshot, size);

    memcpy(candidate, snapshot, size);
    candidate[WIRE_MASK_OFFSET + 3u] = 0x01u;
    reject_without_mutation(candidate, size, snapshot, size);

    memcpy(candidate, snapshot, size);
    candidate[WIRE_CAPTURE_OFFSET] = DC_SPU_PAGE_COUNT + 1u;
    reject_without_mutation(candidate, size, snapshot, size);

    memcpy(candidate, snapshot, size);
    candidate[WIRE_VOICE0_OFFSET] = DC_SPU_PAGE_COUNT + 1u;
    reject_without_mutation(candidate, size, snapshot, size);

    memcpy(candidate, snapshot, size);
    candidate[WIRE_VOICE0_OFFSET + 4u] ^= 8u;
    reject_without_mutation(candidate, size, snapshot, size);

    memcpy(candidate, snapshot, size);
    candidate[WIRE_VOICE0_OFFSET + 8u] = 2u;
    reject_without_mutation(candidate, size, snapshot, size);

    dc_spu_pages_reset();
    assert(dc_spu_page_memory(0) == NULL);
    assert(dc_spu_page_memory(DC_SPU_PAGE_VOICES - 1u) == NULL);
    assert(dc_spu_pages_snapshot_read(snapshot, size));

    page1 = spu_xpage(1);
    page3 = spu_xpage(3);
    assert(page1 && page3 && page1 != page3);
    assert_adpcm(page1, 0x21u);
    assert_adpcm(page3, 0x71u);
    assert(dc_spu_page_memory(0) == page3);
    assert(dc_spu_page_memory(DC_SPU_PAGE_VOICES - 1u) == page1);

    /* The captured-page field survives and applies to a later start. */
    dc_spu_page_start(DC_SPU_PAGE_VOICES - 2u, start0);
    dc_spu_page_key_on(DC_SPU_PAGE_VOICES - 2u, address0);
    assert(dc_spu_page_memory(DC_SPU_PAGE_VOICES - 2u) == page1);

    free(candidate);
    free(snapshot);
}

int main(void)
{
    test_process_init();
    test_page_bounds_and_separation();
    test_capture_binding_and_fallback();
    test_snapshot_round_trip_and_rejection();
    dc_spu_pages_reset();
    return 0;
}
