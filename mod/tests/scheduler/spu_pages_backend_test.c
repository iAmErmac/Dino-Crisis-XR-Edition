#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "test_process.h"
#include "spu.c"

uint64_t s_frame_count;
static uint64_t cycle;
static unsigned irq_count, shadow_resets, shadow_blocks, event_count;
uint64_t psx_get_cycle_count(void) { return cycle; }
void psx_fatal_halt(const char *reason) { fprintf(stderr, "%s\n", reason); abort(); }
void audio_trace_pcm(int tap, const int16_t *data, int frames)
{ (void)tap; assert(data && frames >= 0); }
void audio_trace_event(uint16_t kind, uint32_t a, uint32_t b)
{ (void)kind; (void)a; (void)b; ++event_count; }
void psx_irq_raise(uint32_t bit, uint32_t detail)
{ (void)detail; assert(bit == 9u); ++irq_count; }
bool spu_shadow_enabled(void) { return false; }
void spu_shadow_reset(void) { ++shadow_resets; }
void spu_shadow_process(int16_t *data, int frames)
{ assert(data && frames > 0); ++shadow_blocks; }

static void seed(uint8_t *data, uint8_t packed)
{
    memset(data + 0x100, 0, 32);
    data[0x101] = 4; memset(data + 0x102, packed, 14);
    data[0x111] = 3; memset(data + 0x112, packed, 14);
}
static void start(unsigned voice, int page)
{
    spu_xpage_capture(page);
    spu_write(0x1F801C06u + voice * 16u, 0x21u);
    spu_xpage_capture(0);
    spu_write(voice < 16 ? 0x1F801D88u : 0x1F801D8Au, 1u << (voice & 15));
    assert(voices[voice].cur_addr == 0x100);
}
static void snapshot_put_u32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}
static uint32_t native_voice_field(unsigned voice, unsigned field)
{
    return SPU_REG_COUNT * 2u + voice * SPU_VOICE_WIRE_BYTES + field;
}
static uint32_t private_field(unsigned slot, unsigned field)
{
    return SPU_SNAPSHOT_BASE_BYTES + slot * (16u + 1u + SPU_VOICE_WIRE_BYTES) + 16u + field;
}
static void assert_snapshot_rejected(const uint8_t *core, uint32_t core_size,
                                     const uint8_t *wrapped, uint32_t wrapped_size,
                                     uint32_t offset, uint32_t value, unsigned width,
                                     const uint8_t *live_page)
{
    uint8_t *bad_core = malloc(core_size), *bad_wrapped = malloc(wrapped_size);
    assert(bad_core && bad_wrapped);
    memcpy(bad_core, core, core_size);
    memcpy(bad_wrapped, wrapped, wrapped_size);
    if (width == 1u) {
        bad_core[offset] = (uint8_t)value;
        bad_wrapped[offset] = (uint8_t)value;
    } else {
        snapshot_put_u32(bad_core + offset, value);
        snapshot_put_u32(bad_wrapped + offset, value);
    }
    spu_regs[0] = 0x2468u;
    assert(!spu_snapshot_read(bad_core, core_size));
    assert(spu_regs[0] == 0x2468u);
    assert(dc_spu_page_memory(0) == live_page);
    assert(spu_private_voice_active() & 1u);
    spu_regs[0] = 0x2468u;
    assert(!spu_snapshot_read(bad_wrapped, wrapped_size));
    assert(spu_regs[0] == 0x2468u);
    assert(dc_spu_page_memory(0) == live_page);
    assert(spu_private_voice_active() & 1u);
    free(bad_core);
    free(bad_wrapped);
}
static void test_page_decoder(void)
{
    spu_init(); seed(spu_ram, 0x11); seed(spu_xpage(2), 0x33);
    start(0, 2);
    assert(dc_spu_page_memory(0) == spu_xpage(2));
    decode_block(&voices[0]);
    assert(voices[0].samples[0] == 12288 && !irq_count);
    assert(spu_ram[0x102] == 0x11);
    start(0, 0); assert(!dc_spu_page_memory(0));
    decode_block(&voices[0]); assert(voices[0].samples[0] == 4096);

    start(23, 2); decode_block(&voices[23]);
    assert(voices[23].samples[0] == 12288);
    uint8_t bank[0x120] = {0}; seed(bank, 0x22);
    assert(spu_register_sample_bank(1, bank, sizeof bank));
    assert(spu_bind_next_voice_bank(0, 1));
    start(0, 2); decode_block(&voices[0]);
    assert(voices[0].sample_bank == 1 && voices[0].samples[0] == 8192);
}
static void test_native_irq_and_dma(void)
{
    spu_init(); irq_count = 0; seed(spu_ram, 0x11); seed(spu_xpage(1), 0x33);
    spu_write(0x1F801DA4u, 0x20u);
    spu_write(0x1F801DAAu, 0x8040u);
    start(0, 1); decode_block(&voices[0]); assert(!irq_count);
    start(0, 0); decode_block(&voices[0]); assert(irq_count == 1);
    assert(spu_read(0x1F801DAEu) & 0x40u);
    spu_write(0x1F801DA6u, 0x40u);
    spu_dma_write(0x12345678u);
    assert(spu_ram[0x200] == 0x78 && spu_ram[0x203] == 0x12);
    assert(spu_xpage(1)[0x200] == 0);
}
static void test_snapshot(void)
{
    spu_init(); seed(spu_ram, 0x11); seed(spu_xpage(8), 0x55);
    start(0, 8); decode_block(&voices[0]);
    spu_xpage_capture(3);
    uint32_t size = spu_snapshot_bytes();
    uint8_t *saved = malloc(size), *after = malloc(size), *bad = malloc(size);
    assert(saved && after && bad); spu_snapshot_write(saved);
    assert(spu_snapshot_len_ok(size) && !spu_snapshot_len_ok(size - 1));
    memcpy(bad, saved, size); bad[size - 12] ^= 1;
    assert(!spu_snapshot_validate(bad, size));
    assert(!spu_snapshot_read(bad, size));
    spu_snapshot_write(after); assert(!memcmp(saved, after, size));
    uint32_t core = spu_snapshot_core_bytes();
    memcpy(bad, saved, size); bad[core] ^= 1;
    assert(!spu_snapshot_validate(bad, size));
    assert(!spu_snapshot_read(bad, size));
    spu_snapshot_write(after); assert(!memcmp(saved, after, size));
    spu_init(); assert(!dc_spu_page_memory(0));
    assert(spu_snapshot_validate(saved, size));
    assert(spu_snapshot_read(saved, size));
    assert(dc_spu_page_memory(0) == spu_xpage(8));
    assert(voices[0].samples[0] == 20480 && spu_xpage(8)[0x102] == 0x55);
    spu_write(0x1F801C16u, 0x20u);
    seed(spu_xpage(3), 0x66);
    key_on(2); decode_block(&voices[1]);
    assert(voices[1].samples[0] == 24576);

    uint32_t legacy_size = spu_snapshot_core_bytes();
    uint8_t *legacy = malloc(legacy_size); assert(legacy);
    spu_snapshot_core_write(legacy);
    assert(spu_snapshot_read(legacy, legacy_size));
    assert(!dc_spu_page_memory(0) && !dc_spu_page_memory(1));
    free(legacy); free(saved); free(after); free(bad);
}
static void test_snapshot_validation(void)
{
    uint8_t bank[0x120] = {0};
    uint16_t regs[8] = {0x3FFFu, 0x3FFFu, 0x1000u, 0, 0, 0, 0, 0};
    spu_init(); seed(spu_xpage(6), 0x44); start(0, 6);
    seed(bank, 0x22);
    assert(spu_register_sample_bank(1, bank, sizeof bank));
    assert(spu_private_voice_play(0, 1, regs, 0));
    spu_regs[0] = 0x1357u;
    uint32_t core_size = spu_snapshot_core_bytes(), wrapped_size = spu_snapshot_bytes();
    uint8_t *core = malloc(core_size), *wrapped = malloc(wrapped_size);
    assert(core && wrapped);
    spu_snapshot_core_write(core);
    spu_snapshot_write(wrapped);
    uint8_t *after_validation = malloc(wrapped_size); assert(after_validation);
    assert(spu_snapshot_validate(core, core_size));
    assert(spu_snapshot_validate(wrapped, wrapped_size));
    spu_snapshot_write(after_validation);
    assert(!memcmp(wrapped, after_validation, wrapped_size));
    free(after_validation);
    const uint8_t *live_page = dc_spu_page_memory(0);
    assert(live_page == spu_xpage(6));

    assert_snapshot_rejected(core, core_size, wrapped, wrapped_size,
                             native_voice_field(0, 12u), SPU_SAMPLE_BANKS, 4u, live_page);
    assert_snapshot_rejected(core, core_size, wrapped, wrapped_size,
                             native_voice_field(23, 16u), SPU_SAMPLE_BANKS, 4u, live_page);
    assert_snapshot_rejected(core, core_size, wrapped, wrapped_size,
                             private_field(23, 0u), 4u, 1u, live_page);
    assert_snapshot_rejected(core, core_size, wrapped, wrapped_size,
                             private_field(0, 13u), SPU_SAMPLE_BANKS, 4u, live_page);
    free(core); free(wrapped);
}
static void test_page_clock_snapshot(void)
{
    cycle = 0; spu_init(); seed(spu_xpage(4), 0x11); start(0, 4);
    cycle = 768u * 20u + 400u; spu_catch_up(); spu_output_flush();
    uint32_t size = spu_snapshot_bytes(); uint8_t *saved = malloc(size); assert(saved);
    spu_snapshot_write(saved);
    cycle = 768u * 5000u + 400u;
    assert(spu_snapshot_validate(saved, size));
    assert(spu_snapshot_read(saved, size)); spu_catch_up();
    assert(spu_output_available() == 0);
    assert(dc_spu_page_memory(0) == spu_xpage(4));
    cycle += 367; spu_catch_up(); assert(spu_output_available() == 0);
    ++cycle; spu_catch_up(); assert(spu_output_available() == 1);
    uint32_t core_size = spu_snapshot_core_bytes(); uint8_t *legacy = malloc(core_size); assert(legacy);
    spu_snapshot_core_write(legacy);
    cycle = 768u * 9000u + 123u;
    assert(spu_snapshot_read(legacy, core_size - 4)); spu_catch_up();
    assert(!dc_spu_page_memory(0) && spu_output_available() == 0);
    cycle += 767; spu_catch_up(); assert(spu_output_available() == 0);
    ++cycle; spu_catch_up(); assert(spu_output_available() == 1);
    free(saved); free(legacy);
}
int main(void)
{
    test_process_init();
    test_page_decoder(); test_native_irq_and_dma(); test_snapshot();
    test_snapshot_validation(); test_page_clock_snapshot();
    assert(shadow_resets && shadow_blocks && event_count);
    dc_spu_pages_reset();
    puts("PASS: SPU pages, native banks, IRQ/DMA isolation, snapshot restore and validation");
    return 0;
}
