#include "dc_fk_policy.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
static uint8_t ram[0x200000];
volatile int g_dc_standdown;
static uint32_t generic_forget, private_forget;
uint8_t *memory_get_ram_ptr(void) { return ram; }
void psx_pgxp_forget_register(uint32_t reg) { generic_forget |= 1u << reg; }
void dc_pgxp_forget_register(uint32_t reg) { private_forget |= 1u << reg; }
static void word(uint32_t address, uint32_t value) { memcpy(ram + address, &value, 4); }
int main(void) {
    const uint32_t first = 0x82E00000u, second = 0x82E20000u;
    assert(!dc_fk_bank(0x82DFFFFCu) && !dc_fk_bank(0x82F00000u));
    for (uint32_t i = 0; i < 8; ++i) {
        uint32_t pc = first + i * 0x20000u;
        assert(dc_fk_bank(pc + 0x1FFFCu) == (pc & 0x1FFFFFFFu));
        assert(dc_fk_bank(pc | 0x20000000u) == (pc & 0x1FFFFFFFu));
        assert(dc_fk_address(pc, 0x800E8120u) == pc + 0x120u);
        assert(dc_fk_address(pc, 0x000E8120u) == pc + 0x120u);
        assert(dc_fk_pointer(pc, 0x800E8120u) == pc + 0x120u);
        assert(dc_fk_pointer(pc, 0x000E8120u) == 0x000E8120u);
        assert(dc_fk_address(pc, 0x80100000u) == 0x80100000u);
    }
    CPUState cpu = {0}; cpu.gpr[2] = 0x800E8120u; cpu.gpr[3] = 7; cpu.gpr[8] = 0x800E8120u;
    dc_fk_leave(&cpu, first, second); assert(cpu.gpr[2] == 0x800E8120u && !generic_forget);
    dc_fk_leave(&cpu, first, 0x80010000u);
    assert(cpu.gpr[2] == first + 0x120u && cpu.gpr[3] == 7 && cpu.gpr[8] == 0x800E8120u);
    assert(generic_forget == (1u << 2) && private_forget == generic_forget);
    dc_fk_gsv_reset(); dc_fk_room_tick(42); word(0xB7998u, 0x80010000u);
    uint32_t a = dc_fk_store_word(first, 0x800B7998u, 0x800E8120u); word(0xB7998u, a);
    assert(a == first + 0x120u);
    assert(dc_fk_load_word(0x80010000u, 0xB7998u, a) == 0x80010000u);
    uint32_t b = dc_fk_store_word(second, 0xB7998u, 0x800E8340u); word(0xB7998u, b);
    assert(dc_fk_load_word(first, 0xB7998u, b) == a);
    assert(dc_fk_load_word(second, 0xB7998u, a) == b);
    word(0x10000u + 340u, 0x800B7ACCu); word(0xB56F0u, 0x80020000u); ram[0xB7ACCu + 39u] = 3;
    word(0x20000u + 12u, first + 0x300u);
    assert(dc_fk_special(0x80010000u, 0x148u, b) == a);
    word(0x20000u + 12u, second + 0x300u);
    assert(dc_fk_special(0x80010000u, 0x148u, a) == b);
    word(0x20000u + 12u, 0x800E9000u);
    assert(dc_fk_special(0x80010000u, 0x148u, a) == 0x80010000u);
    assert(dc_fk_special(0, 0x148u, a) == a && dc_fk_special(0x80010000u, 0x150u, a) == a);
    g_dc_standdown = 1; assert(dc_fk_load_word(first, 0xB7890u, 4) == 6);
    assert(dc_fk_load_word(first, 0xB7894u, 4) == 4); g_dc_standdown = 0;
    size_t size = dc_fk_snapshot_bytes(); uint8_t *saved = malloc(size), *bad = malloc(size); assert(saved && bad);
    assert(dc_fk_snapshot_write(saved, size) && dc_fk_snapshot_validate(saved, size));
    dc_fk_gsv_reset(); assert(dc_fk_load_word(first, 0xB7998u, 7) == 7);
    assert(dc_fk_snapshot_read(saved, size) && dc_fk_load_word(first, 0xB7998u, 7) == a);
    dc_fk_room_tick(42); assert(dc_fk_load_word(first, 0xB7998u, 7) == a);
    memcpy(bad, saved, size); bad[24] = 2;
    assert(!dc_fk_snapshot_validate(bad, size) && !dc_fk_snapshot_read(bad, size));
    assert(dc_fk_load_word(first, 0xB7998u, 7) == a);
    assert(!dc_fk_snapshot_read(saved, size - 1) && !dc_fk_snapshot_validate(NULL, size));
    dc_fk_room_tick(43); assert(dc_fk_load_word(first, 0xB7998u, 7) == 7);
    assert(dc_fk_snapshot_read(saved, size));
    dc_fk_room_tick(42); assert(dc_fk_load_word(first, 0xB7998u, 7) == a);
    free(bad); free(saved); return 0;
}
