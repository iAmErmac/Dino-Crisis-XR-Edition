#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu_state.h"
#include "dc_memory_policy.h"
#include "mod_memory.h"
#include "pgxp.h"
#include "pgxp_hooks.h"
#include "pgxp_cells.h"
#include "psx_cyc.h"
#include "psx_memory.h"
#include "test_process.h"

extern void memory_init_nobios(void);
extern uint8_t *memory_get_ram_ptr(void);
extern uint32_t psx_read_word(uint32_t);
extern uint16_t psx_read_half(uint32_t);
extern uint8_t psx_read_byte(uint32_t);
extern void psx_write_word(uint32_t, uint32_t);
extern void psx_write_half(uint32_t, uint16_t);
extern void psx_write_byte(uint32_t, uint8_t);
extern uint32_t psx_mod_memory_snapshot_bytes(void);
extern void psx_mod_memory_snapshot_write(uint8_t *);
extern uint32_t psx_mod_gpu_dma_memory_alloc(uint32_t, uint32_t);
extern int g_dma_exec_depth;
void test_memory_linked_list(void);

enum { PACKED = 0x005000A0u, SWC2_14 = (0x3Au << 26) | (1u << 21) | (14u << 16) };
static void produce(uint32_t addr, int32_t x)
{
    pgxp_gte_push_sxy(x, (80 << 16) | 0x4000, 100, PACKED);
    psx_pgxp_cop2(NULL, SWC2_14, PACKED, addr);
}
static PGXPWordShadow shadow(uint32_t addr)
{
    PGXPWordShadow value;
    assert(pgxp_capture_word_shadow(addr, PACKED, &value));
    return value;
}
static uint32_t segment_alias(uint32_t phys, uint32_t segment)
{
    return phys | segment;
}
static int check_timed_aliases(CPUState *cpu, uint32_t phys,
                               uint8_t expected_byte, uint32_t expected_word)
{
    static const uint32_t segments[] = { 0x00000000u, 0x80000000u, 0xA0000000u };
    int ok = 1;
    for (size_t i = 0; i < sizeof segments / sizeof segments[0]; ++i) {
        const uint32_t address = segment_alias(phys, segments[i]);
        const uint8_t actual_byte = psx_cyc_load_byte(cpu, address, 2, 0);
        const uint32_t actual_word = psx_cyc_lwc2_read(cpu, address);
        if (actual_byte != expected_byte || actual_word != expected_word) {
            ok = 0;
            fprintf(stderr, "timed alias mismatch phys=%08X addr=%08X "
                    "byte=%02X/%02X word=%08X/%08X\n",
                    phys, address, actual_byte, expected_byte,
                    actual_word, expected_word);
        }
    }
    return ok;
}
static void test_precision(void)
{
    pgxp_set_enabled(1); pgxp_set_cpu_mode(1); assert(pgxp_active());
    pgxp_addr_boot(); assert(g_pgxp_addr_on);
    const uint32_t ram = 0x80100000u, arena = 0x80500000u, mod = 0x81010000u;
    const int32_t x1 = (160 << 16) | 0x8000, x2 = (160 << 16) | 0xC000;
    psx_write_word(arena, PACKED); psx_write_word(mod, PACKED);
    produce(ram, x1); produce(arena, x2); produce(mod, x2);
    assert(shadow(ram).valid && shadow(ram).x16 == x1);
    assert(shadow(arena).valid && shadow(arena).x16 == x2);
    assert(shadow(mod).valid && shadow(mod).x16 == x2);
    assert(shadow(arena).address == (arena & 0x1FFFFFFFu));
    assert(shadow(mod).address == (mod & 0x1FFFFFFFu));
    pgxp_checkpoint_begin();
    produce(arena, x1); produce(mod, x1);
    assert(shadow(arena).x16 == x1 && shadow(mod).x16 == x1);
    pgxp_checkpoint_rollback();
    assert(shadow(arena).x16 == x2 && shadow(mod).x16 == x2);
    assert(shadow(ram).x16 == x1);

    int32_t index = px_index(arena); assert(index >= 0);
    g_px_live[index >> 5] |= 1u << (index & 31);
    psx_host_write_word(arena, PACKED);
    assert(!px_is_live(arena) && !shadow(arena).valid && shadow(ram).valid);
    produce(arena, x2);
    psx_host_write_half(arena + 2, (uint16_t)(PACKED >> 16));
    assert(!shadow(arena).valid && shadow(ram).valid);
    produce(arena, x2);
    g_dma_exec_depth = 1; psx_write_byte(arena + 3, (uint8_t)(PACKED >> 24)); g_dma_exec_depth = 0;
    assert(!shadow(arena).valid && shadow(ram).valid);
    const uint32_t scratch = 0x1F800100u;
    index = px_index(scratch); assert(index >= 0);
    g_px_live[index >> 5] |= 1u << (index & 31);
    psx_host_write_word(scratch, PACKED);
    assert(!px_is_live(scratch));
    produce(scratch, x1); assert(shadow(scratch).valid);
    psx_host_write_byte(scratch, (uint8_t)PACKED);
    assert(!shadow(scratch).valid);
    pgxp_invalidate_all();
    assert(!shadow(ram).valid && !shadow(mod).valid);
}

int dc_test_reject_render_reads;
extern int g_psx_render_pass_active;

int main(int argc, char **argv)
{
    test_process_init();
    psx_ram_reset_size_request();
    memory_init_nobios();
    if (argc == 2) {
        dc_test_reject_render_reads = 1;
        g_psx_render_pass_active = 1;
        switch (atoi(argv[1])) {
        case 1: (void)psx_read_byte(0x1F801800u); break;
        case 2: (void)psx_read_half(0x1F801800u); break;
        case 4: (void)psx_read_word(0x1F801800u); break;
        default: abort();
        }
        return 9;
    }
    assert(memory_get_ram_bytes() == 0x200000u);
    CPUState cpu = {0}; cpu.read_word = psx_read_word; cpu.read_half = psx_read_half;
    cpu.read_byte = psx_read_byte; cpu.write_word = psx_write_word;
    cpu.write_half = psx_write_half; cpu.write_byte = psx_write_byte;

    psx_write_word(0x80001000u, 0x12345678u);
    psx_write_word(0x80401000u, 0x89ABCDEFu);
    assert(psx_read_word(0x80001000u) == 0x12345678u);
    assert(psx_read_word(0xA0401000u) == 0x89ABCDEFu);
    assert(psx_mod_gpu_dma_resolve_address(0x00401000u) == 0x00401000u);
    assert(psx_mod_gpu_dma_resolve_address(0x00BFFFFCu) == 0x00BFFFFCu);
    assert(psx_gpu_packet_key(0x80401000u) == 0x00401000u);
    assert(psx_gpu_packet_key(0x80201000u) == 0x00001000u);
    assert(psx_cyc_load_word(&cpu, 0x80401000u, 2, 0) == 0x89ABCDEFu);
    assert(psx_cyc_load_word(&cpu, 0x80001000u, 2, 0) == 0x12345678u);
    uint32_t offset;
    assert(!psx_ram_resolve(0x80401000u, 4, &offset));
    assert(psx_ram_resolve(0x80201000u, 4, &offset) && offset == 0x1000);

    psx_write_half(0x80401002u, 0x7654);
    psx_write_byte(0x80401001u, 0x32);
    assert(psx_read_word(0x80401000u) == 0x765432EFu);
    assert(psx_read_half(0x80401002u) == 0x7654);
    assert(psx_read_byte(0x80401001u) == 0x32);
    assert(psx_cyc_load_half(&cpu, 0x80401002u, 2, 0) == 0x7654);
    assert(psx_read_word(0x80001000u) == 0x12345678u);

    /* Timed byte/LWC2 reads must keep allocated extra memory ahead of the
     * retail 2 MiB mirror fold, for every cached/uncached guest alias. */
    {
        const uint32_t base_word = 0x11223344u;
        const uint32_t folded_mod_word = 0x55667788u;
        memcpy(memory_get_ram_ptr() + 0x00001200u, &base_word, sizeof base_word);
        memcpy(memory_get_ram_ptr() + 0x00101200u,
               &folded_mod_word, sizeof folded_mod_word);
    }
    psx_write_word(0x80401200u, 0xA1B2C3D4u);
    psx_write_word(0x81001200u, 0xE5F60718u);
    {
        int timed_ok = 1;
        timed_ok &= check_timed_aliases(&cpu, 0x00001200u, 0x44u, 0x11223344u);
        timed_ok &= check_timed_aliases(&cpu, 0x00201200u, 0x44u, 0x11223344u);
        timed_ok &= check_timed_aliases(&cpu, 0x00401200u, 0xD4u, 0xA1B2C3D4u);
        timed_ok &= check_timed_aliases(&cpu, 0x01001200u, 0x18u, 0xE5F60718u);
        assert(timed_ok);
    }

    /* An allocated generic aperture must not take ownership of game packets. */
    assert(psx_mod_gpu_dma_memory_alloc(64, 16) == 0x80800000u);
    psx_write_word(0x80800000u, 0x11223344u);
    assert(psx_read_word(0x80800000u) == 0x11223344u);
    uint32_t bytes = psx_mod_memory_snapshot_bytes();
    uint8_t *snapshot = malloc(bytes); assert(snapshot && bytes == 16 + 64);
    psx_mod_memory_snapshot_write(snapshot);
    for (uint32_t i = 16; i < bytes; ++i) assert(snapshot[i] == 0);
    free(snapshot);

    psx_write_word(0x81000100u, 0x55667788u);
    assert(psx_read_word(0xA1000100u) == 0x55667788u);
    psx_host_write_half(0x81000102u, 0xABCD);
    assert(psx_read_word(0x81000100u) == 0xABCD7788u);
    assert(psx_read_word(0x80001000u) == 0x12345678u);
    /* Kernel-table probes outside allocated storage retain console bus behavior. */
    assert(!dc_memory_extra_address(0x1A5A022Eu));
    assert(psx_read_word(0x5A5A022Eu) == 0);
    assert(psx_read_half(0x5A5A022Eu) == 0);
    assert(psx_read_byte(0x5A5A022Eu) == 0);
    assert(dc_memory_extra_address(0x00401000u));
    assert(dc_memory_extra_address(0x01000100u));
    test_precision();
    test_memory_linked_list();
    puts("PASS: scalar, host and inline loads preserve game extra-memory precedence");
    return 0;
}
