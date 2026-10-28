#include "dc_game_entry.h"

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { RAM_BYTES = 0x00200000u };
static uint8_t test_ram[RAM_BYTES];
static unsigned room_calls;
static unsigned choice_calls;
static unsigned aim_calls;
static uint32_t room_fn[3];
static uint32_t room_task[3];
static uint32_t aim_player;

uint8_t *memory_get_ram_ptr(void)
{
    return test_ram;
}

void dc_room_script_op_hook(uint32_t fn, uint32_t task)
{
    assert(room_calls < 3u);
    room_fn[room_calls] = fn;
    room_task[room_calls] = task;
    room_calls++;
}

void dc_vchoice_hook(void)
{
    choice_calls++;
}

void dc_aim_point_hook(uint32_t player)
{
    aim_calls++;
    aim_player = player;
}

static void set_env(const char *name, const char *value)
{
#ifdef _WIN32
    assert(_putenv_s(name, value) == 0);
#else
    assert(setenv(name, value, 1) == 0);
#endif
}

static void clear_env(const char *name)
{
#ifdef _WIN32
    assert(_putenv_s(name, "") == 0);
#else
    assert(unsetenv(name) == 0);
#endif
}

int main(void)
{
    CPUState cpu = {0};
    memset(test_ram, 0, sizeof test_ram);
    memset((void *)g_dc_fn_ring, 0, sizeof g_dc_fn_ring);
    memset((void *)g_fn_ring2, 0, sizeof g_fn_ring2);
    g_dc_fn_pos = 0;
    g_fn_ring2_pos = 0;
    set_env("DC_FNHIST", "1");

    for (uint32_t i = 0; i < 33u; ++i) dc_game_entry(&cpu, 0x80010000u + i);
    assert(g_dc_fn_pos == 33u);
    assert(g_dc_fn_ring[0] == 0x80010020u);
    assert(g_dc_fn_ring[31] == 0x8001001Fu);
    assert(g_fn_ring2_pos == 33u);
    assert(g_fn_ring2[0] == 0x80010000u);
    assert(g_fn_ring2[32] == 0x80010020u);

    set_env("DC_NULL_MESH_GUARD", "1");
    test_ram[0x10] = 1; test_ram[0x11] = 2; test_ram[0x12] = 3; test_ram[0x13] = 4;
    cpu.gpr[4] = 0;
    dc_game_entry(&cpu, 0x800756F4u);
    assert(test_ram[0x10] == 0 && test_ram[0x11] == 0 && test_ram[0x12] == 0 && test_ram[0x13] == 0);
    test_ram[0x10] = 9;
    cpu.gpr[4] = 1;
    dc_game_entry(&cpu, 0x800756F4u);
    assert(test_ram[0x10] == 9);

    cpu.gpr[4] = 0x80012340u;
    dc_game_entry(&cpu, 0x80058834u);
    dc_game_entry(&cpu, 0x800570F8u);
    dc_game_entry(&cpu, 0x80057EDCu);
    assert(room_calls == 3u);
    assert(room_fn[0] == 0x80058834u && room_fn[1] == 0x800570F8u && room_fn[2] == 0x80057EDCu);
    assert(room_task[0] == 0x80012340u && room_task[1] == 0x80012340u && room_task[2] == 0x80012340u);

    dc_game_entry(&cpu, 0x8001FCECu);
    assert(choice_calls == 1u);
    cpu.gpr[4] = 0x800B928Cu;
    dc_game_entry(&cpu, 0x80052378u);
    assert(aim_calls == 1u && aim_player == 0x800B928Cu);

    set_env("DC_NOTS_ONECAM", "1");
    cpu.gpr[4] = 0x80000100u;
    dc_game_entry(&cpu, 0x80026B14u);
    assert(test_ram[0x100u + 114u] == 4u);
    test_ram[RAM_BYTES - 1u] = 0x5Au;
    cpu.gpr[4] = 0x800FFFF0u;
    dc_game_entry(&cpu, 0x80026B14u);
    assert(test_ram[RAM_BYTES - 1u] == 0x5Au);
    test_ram[0x72u] = 0xA5u;
    cpu.gpr[4] = 0x80200000u;
    dc_game_entry(&cpu, 0x80026B14u);
    assert(test_ram[0x72u] == 0xA5u);
    test_ram[0x10006Du] = 0x5Au;
    cpu.gpr[4] = 0x80FFFFFFu;
    dc_game_entry(&cpu, 0x80026B14u);
    assert(test_ram[0x10006Du] == 0x5Au);

    set_env("DC_BG_INSTANT", "1");
    set_env("DC_BG_INSTANT_N", "3");
    cpu.gpr[4] = 0x80000200u;
    test_ram[0x200u + 114u] = 1u;
    test_ram[0x200u + 115u] = 6u;
    dc_game_entry(&cpu, 0x8002731Cu);
    assert(test_ram[0x200u + 115u] == 3u);
    test_ram[0x200u + 115u] = 2u;
    dc_game_entry(&cpu, 0x8002731Cu);
    assert(test_ram[0x200u + 115u] == 2u);

    clear_env("DC_NULL_MESH_GUARD");
    clear_env("DC_NOTS_ONECAM");
    clear_env("DC_BG_INSTANT");
    clear_env("DC_BG_INSTANT_N");
    clear_env("DC_FNHIST");
    return 0;
}
