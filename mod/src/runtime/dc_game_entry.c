#include "dc_game_entry.h"

#include <stdint.h>
#include <stdlib.h>

enum { DC_RAM_BYTES = 0x00200000u };

extern uint8_t *memory_get_ram_ptr(void);
extern void dc_room_script_op_hook(uint32_t fn, uint32_t task);
extern void dc_vchoice_hook(void);
extern void dc_aim_point_hook(uint32_t player);

volatile uint32_t g_dc_fn_ring[DC_GAME_ENTRY_RING_CAP];
volatile uint32_t g_dc_fn_pos;
volatile uint32_t g_fn_ring2[DC_GAME_ENTRY_RING2_CAP];
volatile uint32_t g_fn_ring2_pos;

static int env_enabled(const char *name, int default_value)
{
    const char *value = getenv(name);
    if (!value || !*value) return default_value;
    return value[0] != '0';
}

static int env_cached(const char *name, int default_value, int *cache)
{
    if (*cache < 0) *cache = env_enabled(name, default_value);
    return *cache;
}

static void diagnostic_ring2(uint32_t func_addr)
{
    static int enabled = -1;

    if (enabled < 0) {
        const char *value = getenv("DC_FNHIST");
        enabled = value && *value == '1';
    }
    if (enabled) g_fn_ring2[g_fn_ring2_pos++ & (DC_GAME_ENTRY_RING2_CAP - 1u)] = func_addr;
}

static int main_ram_pointer(uint32_t value, uint32_t bytes, uint32_t *offset)
{
    uint32_t physical;

    if ((value >> 24) != 0x80u) return 0;
    physical = value - 0x80000000u;
    if (physical > DC_RAM_BYTES - bytes) return 0;
    *offset = physical;
    return 1;
}

static void null_mesh_guard(void)
{
    static int enabled = -1;
    uint8_t *ram;

    if (!env_cached("DC_NULL_MESH_GUARD", 1, &enabled)) return;
    ram = memory_get_ram_ptr();
    if (!ram || !(ram[0x10] | ram[0x11] | ram[0x12] | ram[0x13])) return;
    ram[0x10] = 0;
    ram[0x11] = 0;
    ram[0x12] = 0;
    ram[0x13] = 0;
}

static void one_camera_policy(CPUState *cpu)
{
    static int enabled = -1;
    const char *value;
    uint32_t offset;
    uint8_t *ram;

    if (enabled < 0) {
        value = getenv("DC_NOTS_ONECAM");
        enabled = value && *value && value[0] == '1';
    }
    if (!cpu || !enabled) return;
    if (!main_ram_pointer(cpu->gpr[4], 115u, &offset)) return;
    ram = memory_get_ram_ptr();
    if (!ram) return;
    ram[offset + 114u] |= 4u;
}

static int bg_instant_count(void)
{
    static int enabled = -1;
    static int count = 1;
    const char *value;

    if (enabled < 0) {
        enabled = env_enabled("DC_BG_INSTANT", 1);
        value = getenv("DC_BG_INSTANT_N");
        if (value && *value) count = atoi(value);
        if (count < 1) count = 1;
        if (count > 6) count = 6;
    }
    if (!enabled) return 0;
    return count;
}

static void background_policy(CPUState *cpu)
{
    uint32_t offset;
    uint8_t *ram;
    int count = bg_instant_count();

    if (!cpu || !count || !main_ram_pointer(cpu->gpr[4], 116u, &offset)) return;
    ram = memory_get_ram_ptr();
    if (!ram || !(ram[offset + 114u] & 1u)) return;
    if (ram[offset + 115u] > (uint8_t)count) ram[offset + 115u] = (uint8_t)count;
}

void dc_game_entry(CPUState *cpu, uint32_t func_addr)
{
    g_dc_fn_ring[g_dc_fn_pos++ & (DC_GAME_ENTRY_RING_CAP - 1u)] = func_addr;
    diagnostic_ring2(func_addr);

    if (func_addr == 0x800756F4u && cpu && cpu->gpr[4] == 0u) {
        null_mesh_guard();
    }

    if (cpu && (func_addr == 0x80058834u || func_addr == 0x800570F8u ||
                func_addr == 0x80057EDCu)) {
        dc_room_script_op_hook(func_addr, cpu->gpr[4]);
    }

    if (func_addr == 0x80052378u && cpu) {
        dc_aim_point_hook(cpu->gpr[4]);
    }

    if (func_addr == 0x8001FCECu) {
        dc_vchoice_hook();
    }

    if (func_addr == 0x80026B14u) one_camera_policy(cpu);
    if (func_addr == 0x8002731Cu) background_policy(cpu);

}
