#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "dc_dma_policy.h"
#include "test_process.h"

uint32_t g_ot_lo, g_ot_hi;
int g_ot_watch, g_dc_pass_id = 7, g_dc_dma_pass;
static unsigned begin_count, submission_count, visits, finishes, otc_count;
static uint32_t last_mask, last_otc, sequence[16], sequence_count;
void dc_meshrep_dma_begin(void) { ++begin_count; sequence[sequence_count++] = 1; }
void dcvk_note_submission(void) { ++submission_count; sequence[sequence_count++] = 2; }
void dc_meshrep_dma_at(uint32_t a) { ++visits; sequence[sequence_count++] = a; }
void dc_meshrep_dma(uint32_t mask) { ++finishes; last_mask = mask; sequence[sequence_count++] = 0xF0000000u | mask; }
void dc_meshrep_otc(uint32_t a) { ++otc_count; last_otc = a; }
void psx_fatal_halt(const char *reason) { (void)reason; abort(); }

int main(void)
{
    test_process_init();
    uint8_t wire[DC_DMA_POLICY_WIRE_BYTES], again[sizeof wire], bad[sizeof wire];
    dc_dma_policy_cancel(); assert(dc_dma_policy_write(wire, sizeof wire, 0));
    dc_dma_policy_begin();
    assert(begin_count == 1 && submission_count == 1 && g_dc_dma_pass == 7 && g_ot_watch);
    assert(sequence[0] == 1 && sequence[1] == 2);
    dc_dma_policy_node(0x800B5850u); dc_dma_policy_node(0x802B6850u);
    dc_dma_policy_node(0x804B5850u);
    assert(visits == 2 && sequence[2] == 0xB5850u && sequence[3] == 0xB6850u);
    assert(g_ot_lo == 0xB5850u && g_ot_hi == 0x4B5850u);
    assert(dc_dma_policy_write(wire, sizeof wire, 1));
    assert(wire[0] == 1 && wire[4] == 1 && wire[8] == 3);
    assert(!dc_dma_policy_validate(wire, sizeof wire, 0));
    dc_dma_policy_finish(); assert(finishes == 1 && last_mask == 3);
    assert(dc_dma_policy_read(wire, sizeof wire, 1));
    assert(begin_count == 1 && submission_count == 1 && visits == 2 && finishes == 1);
    assert(dc_dma_policy_write(again, sizeof again, 1)); assert(!memcmp(wire, again, sizeof wire));
    memcpy(bad, wire, sizeof bad); bad[0] = 2;
    assert(!dc_dma_policy_read(bad, sizeof bad, 1));
    memcpy(bad, wire, sizeof bad); bad[8] = 4;
    assert(!dc_dma_policy_read(bad, sizeof bad, 1));
    for (size_t n = 0; n < sizeof wire; ++n) assert(!dc_dma_policy_read(wire, n, 1));
    assert(!dc_dma_policy_read(NULL, sizeof wire, 1));
    assert(dc_dma_policy_write(again, sizeof again, 1)); assert(!memcmp(wire, again, sizeof wire));
    dc_dma_policy_finish(); assert(finishes == 2 && last_mask == 3);
    dc_dma_policy_begin(); dc_dma_policy_node(0xB5850u); dc_dma_policy_cancel();
    assert(finishes == 2 && !g_ot_watch);
    assert(dc_dma_policy_write(wire, sizeof wire, 0)); assert(wire[8] == 0);
    dc_dma_policy_begin(); dc_dma_policy_finish(); assert(finishes == 3 && last_mask == 0);
    dc_dma_policy_otc(0xB684Cu); assert(otc_count == 1 && last_otc == 0xB684Cu);
    memset(bad, 0xA5, sizeof bad); assert(!dc_dma_policy_write(bad, sizeof bad, 1));
    for (size_t n = 0; n < sizeof bad; ++n) assert(bad[n] == 0xA5);
    return 0;
}
