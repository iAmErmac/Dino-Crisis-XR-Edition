#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "cpu_state.h"
#include "memory_policy_frontend.h"

enum { AMMO_BASE = 0xB94CEu };

static int drawlist_calls;
static int ammo_calls;
static struct {
    uint32_t phys;
    int old_value;
    int new_value;
} ammo_notes[4];
static int damage_notes;
static int damage_slot;
static int damage_old;
static int damage_new;
static float damage_scale;

int g_opt_focus_mode;

static void unexpected_call(void)
{
    abort();
}

#ifndef DC_TEST_REAL_MEMORY
int g_wtrace_any;
volatile int g_card_data_armed;
uint32_t g_debug_last_store_pc;
uint32_t g_debug_current_func_addr;
CPUState *debug_cpu_ptr;
uint64_t s_frame_count = 1;

void psx_fatal_halt(const char *reason)
{
    (void)reason;
    abort();
}
#endif

void memory_policy_frontend_reset(void)
{
    drawlist_calls = 0;
    ammo_calls = 0;
    memset(ammo_notes, 0, sizeof ammo_notes);
    damage_notes = 0;
    damage_slot = 0;
    damage_old = 0;
    damage_new = 0;
    damage_scale = 0.0f;
}

int memory_policy_frontend_drawlist_calls(void) { return drawlist_calls; }
int memory_policy_frontend_ammo_calls(void) { return ammo_calls; }
int memory_policy_frontend_ammo_old(unsigned index)
{
    assert(index < sizeof ammo_notes / sizeof ammo_notes[0]);
    return ammo_notes[index].old_value;
}
int memory_policy_frontend_ammo_new(unsigned index)
{
    assert(index < sizeof ammo_notes / sizeof ammo_notes[0]);
    return ammo_notes[index].new_value;
}
int memory_policy_frontend_damage_calls(void) { return damage_notes; }
int memory_policy_frontend_damage_slot(void) { return damage_slot; }
int memory_policy_frontend_damage_old(void) { return damage_old; }
int memory_policy_frontend_damage_new(void) { return damage_new; }
float memory_policy_frontend_damage_scale(void) { return damage_scale; }

void dc_drawlist_watch(uint32_t phys, uint32_t value, int width)
{
    assert(phys == AMMO_BASE);
    assert(value == 0x7856u);
    assert(width == 2);
    ++drawlist_calls;
}

void dc_ammo_write_note(uint32_t phys, int old_value, int new_value)
{
    assert(ammo_calls < (int)(sizeof ammo_notes / sizeof ammo_notes[0]));
    assert(phys == AMMO_BASE + (uint32_t)ammo_calls);
    assert(old_value == (ammo_calls == 0 ? 0x34 : 0x12));
    assert(new_value == (ammo_calls == 0 ? 0x56 : 0x78));
    ammo_notes[ammo_calls].phys = phys;
    ammo_notes[ammo_calls].old_value = old_value;
    ammo_notes[ammo_calls].new_value = new_value;
    ++ammo_calls;
}

void dc_dmg_note(int slot, int oldhp, int newhp, float scale)
{
    assert(slot == 0);
    assert(oldhp == 100);
    assert(newhp == 80);
    assert(scale == 2.0f);
    ++damage_notes;
    damage_slot = slot;
    damage_old = oldhp;
    damage_new = newhp;
    damage_scale = scale;
}

int dc_strafe_speed_scale(int value, int *out)
{
    (void)value;
    (void)out;
    unexpected_call();
    return 0;
}
int dc_cut_active(void) { unexpected_call(); return 0; }
int dc_m5_speed_scale(int value, int old_value, int *out)
{
    (void)value;
    (void)old_value;
    (void)out;
    unexpected_call();
    return 0;
}
int dc_cut_camera(void) { return 0; }
int dc_fire_recent(void) { unexpected_call(); return 0; }
float dc_damage_mult(int weapon, float distance)
{
    assert(weapon == 0x10);
    assert(distance >= 0.0f);
    return 2.0f;
}
int dc_dart_now(void) { return 0; }
float dc_focus_damage_scale(void) { unexpected_call(); return 1.0f; }

#ifndef DC_TEST_REAL_MEMORY
void debug_server_trace_write_check(uint32_t phys, uint32_t old_value,
                                    uint32_t new_value, uint8_t width)
{
    (void)phys;
    (void)old_value;
    (void)new_value;
    (void)width;
    unexpected_call();
}
void debug_server_trace_mmio_write(uint32_t addr, uint32_t value, uint8_t width)
{
    (void)addr;
    (void)value;
    (void)width;
    unexpected_call();
}
int card_data_writes_check(uint32_t phys, uint32_t value, uint8_t width)
{
    (void)phys;
    (void)value;
    (void)width;
    unexpected_call();
    return 0;
}
#endif

void dc_ots_last_yaw_write(int *yaw, int *age)
{
    (void)yaw;
    (void)age;
    unexpected_call();
}
int dc_coll_count(void) { unexpected_call(); return 0; }
int dc_room_current(void) { unexpected_call(); return 0; }
#ifndef DC_TEST_REAL_MEMORY
void psx_write_word(uint32_t addr, uint32_t value)
{
    (void)addr;
    (void)value;
    unexpected_call();
}
void psx_write_half(uint32_t addr, uint16_t value)
{
    (void)addr;
    (void)value;
    unexpected_call();
}
void psx_write_byte(uint32_t addr, uint8_t value)
{
    (void)addr;
    (void)value;
    unexpected_call();
}
uint32_t psx_read_word(uint32_t addr)
{
    (void)addr;
    unexpected_call();
    return 0;
}
#endif
