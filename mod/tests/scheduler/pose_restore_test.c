#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dc_draw_pose.h"
#include "test_process.h"

uint64_t s_frame_count;
int g_savestate_loads, g_dc_fire_btn;
const unsigned char *dc_embedded(const char *name, unsigned *size)
{ (void)name; (void)size; abort(); }
void psx_fatal_halt(const char *reason) { (void)reason; abort(); }
int pt_anim_smooth(void) { abort(); }
int dc_orig_pure(void) { abort(); }
int dc_aim_walk_mode(void) { abort(); }
int dc_aim_walk_raw(void) { abort(); }
int dc_aim_walk_pose_now(void) { abort(); }
int dc_aim_ready_now(void) { abort(); }
int dc_ots_move_intent(void) { abort(); }

extern void dc_ladder_draw_enter(CPUState *cpu);
extern void dc_grenade_defer_explode(uint32_t object, const int16_t position[3]);
#include "dc_guest_registers.h"
static uint8_t ram[0x200000];
uint8_t *memory_get_ram_ptr(void) { return ram; }
static void seed(uint32_t address, int16_t *saved, int16_t *wrote, unsigned count)
{
    for (unsigned i = 0; i < count; ++i) { saved[i] = (int16_t)(100 + i); wrote[i] = (int16_t)(300 + i); }
    memcpy(ram + address, wrote, count * sizeof *wrote);
}
static void check(uint32_t address, const int16_t *saved, unsigned count)
{
    assert(!memcmp(ram + address, saved, count * sizeof *saved));
}
int main(void)
{
    test_process_init();
    CPUState cpu = {0};
    DcDrawPose pose = {0}, after;
    pose.applied = pose.papplied = pose.gapplied = pose.t0applied = pose.rapplied = 1;
    seed(0xB928C, pose.saved, pose.wrote, 9);
    seed(0xB94EC, pose.t0saved, pose.t0wrote, 16);
    seed(0xB94EC + 172 * 8, pose.psaved, pose.pwrote, 9);
    seed(0xB94EC + 68, pose.rsaved, pose.rwrote, 3);
    for (unsigned i = 0; i < 2; ++i) for (unsigned j = 0; j < 3; ++j)
        seed(0xB94EC + 172 * ((i ? 12 : 9) + j), pose.gsaved[i][j], pose.gwrote[i][j], 16);
    /* A changed guest matrix must survive cleanup. */
    ram[0xB94EC + 172 * 10] ^= 1;
    uint8_t changed[32]; memcpy(changed, ram + 0xB94EC + 172 * 10, sizeof changed);
    dc_ladder_draw_restore(&cpu, &pose);
    check(0xB928C, pose.saved, 9); check(0xB94EC, pose.t0saved, 16);
    check(0xB94EC + 172 * 8, pose.psaved, 9); check(0xB94EC + 68, pose.rsaved, 3);
    for (unsigned i = 0; i < 2; ++i) for (unsigned j = 0; j < 3; ++j)
        if (i || j != 1) check(0xB94EC + 172 * ((i ? 12 : 9) + j), pose.gsaved[i][j], 16);
    assert(!memcmp(changed, ram + 0xB94EC + 172 * 10, sizeof changed));
    dc_ladder_draw_capture(&after);
    assert(!after.applied && !after.papplied && !after.gapplied && !after.rapplied && !after.t0applied);
    memset(&pose, 0, sizeof pose); pose.ladder_applied = 1;
    seed(0xB94EC + 68, pose.ladder_saved, pose.ladder_wrote, 3);
    dc_ladder_draw_restore(&cpu, &pose); check(0xB94EC + 68, pose.ladder_saved, 3);
    dc_ladder_draw_capture(&after); assert(!after.ladder_applied);
    memset(ram, 0, sizeof ram);
    ram[0xB92C8] = 4;
    *(int16_t *)(ram + 0xB93DE) = 2;
    *(uint32_t *)(ram + 0xB94EC + 56) = 0x800B928C;
    *(uint32_t *)(ram + 0xB92FC) = 0x80010000;
    *(uint32_t *)(ram + 0xB9300) = 0x80020008;
    *(uint32_t *)(ram + 0x20000) = 0x80010000;
    *(uint32_t *)(ram + 0x20004) = 8;
    for (unsigned i = 0; i < 8; ++i) *(int16_t *)(ram + 0x10000 + 80 * i + 2) = (int16_t)(-1200 - 150 * i);
    *(int16_t *)(ram + 0xB94EC + 72) = 400;
    dc_ladder_draw_enter(&cpu);
    dc_ladder_draw_capture(&pose);
    assert(pose.ladder_applied && pose.ladder_saved[2] == 400 && pose.ladder_wrote[2] == 80);
    /* Another draw can replace the shared scratch without losing this pose. */
    ram[0xB92C8] = 0;
    dc_ladder_draw_enter(&cpu);
    dc_ladder_draw_capture(&after); assert(!after.ladder_applied);
    dc_ladder_draw_restore(&cpu, &pose);
    assert(*(int16_t *)(ram + 0xB94EC + 72) == 400);
    const int16_t explosion_position[3] = {-123, 456, 789};
    ram[0x12000 + 39] = 12; ram[0x12000 + 60] = 1;
    dc_grenade_defer_explode(0x80012000, explosion_position);
    assert(dc_grenade_take() == 0x12000);
    for (unsigned i = 0; i < 3; ++i)
        assert(*(int16_t *)(ram + 0x12000 + 32 + 2 * i) == explosion_position[i]);
    assert(!dc_grenade_take());
    dc_grenade_complete(0x12000); assert(ram[0x12000 + 60] == 3);
    dc_grenade_defer_explode(0x12000, explosion_position); assert(!dc_grenade_take());
    ram[0x12000 + 60] = 1;
    dc_grenade_defer_explode(0x12000, explosion_position); ++g_savestate_loads;
    assert(!dc_grenade_take() && ram[0x12000 + 60] == 1);
    puts("PASS: actual ladder and strafe cleanup preserves guest edits");
    return 0;
}
