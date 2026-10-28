#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dc_memory_policy.h"
#include "memory_policy_frontend.h"
#include "test_process.h"

enum { RAM_BYTES = 0x200000u, SCRATCHPAD_BYTES = 0x400u };
enum { ARENA_BASE = 0x00400000u, MOD_BASE = 0x01000000u };
enum { ARENA_BYTES = 1u << 20, MOD_BYTES = 2u << 20 };
enum { NODE_BYTES = 0xACu, NODE_COUNT = 2048u, NODE_FLAGS = 0x30u };
enum { AMMO_BASE = 0xB94CEu, FOCUS_BASE = 0xB7ACCu, ACTOR_BYTES = 608u };

static uint8_t test_ram[RAM_BYTES];
static uint8_t test_scratchpad[SCRATCHPAD_BYTES];

uint8_t *memory_get_ram_ptr(void) { return test_ram; }
uint8_t *memory_get_scratchpad_ptr(void) { return test_scratchpad; }
uint32_t memory_get_ram_bytes(void) { return RAM_BYTES; }

extern uint32_t dc_node_alloc_extra(void);
extern const uint8_t *memory_mod_ptr(uint32_t phys, uint32_t len);
extern uint32_t memory_mod_word(uint32_t phys);

static void set_env(const char *name, const char *value)
{
#ifdef _WIN32
    assert(_putenv_s(name, value) == 0);
#else
    assert(setenv(name, value, 1) == 0);
#endif
}

static uint32_t load_u32(const uint8_t *p)
{
    uint32_t value;
    memcpy(&value, p, sizeof value);
    return value;
}

static void store_u32(uint8_t *p, uint32_t value)
{
    memcpy(p, &value, sizeof value);
}

static void test_extra_memory_bounds(void)
{
    uint8_t *arena_first;
    uint8_t *arena_last;
    uint8_t *mod_first;
    uint8_t *mod_last;

    assert(dc_memory_extra_address(ARENA_BASE));
    assert(dc_memory_extra_address(MOD_BASE));
    assert(!dc_memory_extra_address(ARENA_BASE - 1u));
    assert(!dc_memory_extra_address(0x1F000000u));

    arena_first = dc_memory_extra_ptr(ARENA_BASE, 4u);
    arena_last = dc_memory_extra_ptr(ARENA_BASE + ARENA_BYTES - 4u, 4u);
    assert(arena_first && arena_last && arena_first != arena_last);
    assert(!dc_memory_extra_ptr(ARENA_BASE + ARENA_BYTES, 1u));
    assert(!dc_memory_extra_ptr(ARENA_BASE + ARENA_BYTES - 1u, 2u));
    assert(!dc_memory_extra_ptr(UINT32_MAX - 3u, 4u));

    mod_first = dc_memory_extra_ptr(MOD_BASE, 4u);
    mod_last = dc_memory_extra_ptr(MOD_BASE + MOD_BYTES - 4u, 4u);
    assert(mod_first && mod_last && mod_first != mod_last);
    assert(!dc_memory_extra_ptr(MOD_BASE + MOD_BYTES, 1u));
    assert(!dc_memory_extra_ptr(MOD_BASE + MOD_BYTES - 1u, 2u));

    arena_first[0] = 0xA5;
    mod_first[0] = 0x5A;
    assert(arena_first[0] == 0xA5);
    assert(mod_first[0] == 0x5A);
    assert(memory_mod_word(MOD_BASE) == 0x0000005Au);
}

static void test_node_flags_and_reuse(void)
{
    const uint32_t node_base = MOD_BASE + MOD_BYTES - 0x100000u;
    const uint32_t first = dc_node_alloc_extra();
    const uint8_t *first_node;

    assert(first == (0x80000000u | node_base));
    first_node = memory_mod_ptr(first & 0x1FFFFFFFu, NODE_BYTES);
    assert(first_node);
    assert(load_u32(first_node + NODE_FLAGS) == 3u);

    store_u32((uint8_t *)first_node + NODE_FLAGS, 1u);
    store_u32((uint8_t *)first_node + 0x20u, 0xC0DEC0DEu);
    for (unsigned i = 1; i < NODE_COUNT; ++i) {
        const uint32_t node = dc_node_alloc_extra();
        assert(node == (0x80000000u | (node_base + i * NODE_BYTES)));
    }
    assert(dc_node_alloc_extra() == 0u);
    first_node = memory_mod_ptr(node_base, NODE_BYTES);
    assert(load_u32(first_node + NODE_FLAGS) == 1u);
    assert(load_u32(first_node + 0x20u) == 0xC0DEC0DEu);

    store_u32((uint8_t *)first_node + NODE_FLAGS, 0u);
    assert(dc_node_alloc_extra() == first);
    first_node = memory_mod_ptr(node_base, NODE_BYTES);
    assert(load_u32(first_node + NODE_FLAGS) == 3u);
    assert(load_u32(first_node + 0x20u) == 0u);
}

static void test_prewrite_ammo_observation(void)
{
    uint32_t value = 0x7856u;

    test_ram[AMMO_BASE] = 0x34;
    test_ram[AMMO_BASE + 1u] = 0x12;
    memory_policy_frontend_reset();

    assert(dc_memory_policy_write(AMMO_BASE, &value, 2u) == 1);
    assert(value == 0x7856u);
    assert(test_ram[AMMO_BASE] == 0x34);
    assert(test_ram[AMMO_BASE + 1u] == 0x12);
    assert(memory_policy_frontend_drawlist_calls() == 1);
    assert(memory_policy_frontend_ammo_calls() == 2);
    assert(memory_policy_frontend_ammo_old(0) == 0x34 &&
           memory_policy_frontend_ammo_new(0) == 0x56);
    assert(memory_policy_frontend_ammo_old(1) == 0x12 &&
           memory_policy_frontend_ammo_new(1) == 0x78);
}

static void test_focus_halfword_policy(void)
{
    const uint32_t phys = FOCUS_BASE + 280u;
    uint32_t value = 90u;

    test_ram[0xB92C8u] = 1u;
    test_ram[0xB92C9u] = 6u;
    test_ram[0xB92ACu] = 0u;
    test_ram[0xB92ADu] = 0u;
    test_ram[0xB92B0u] = 0u;
    test_ram[0xB92B1u] = 0u;
    test_ram[phys] = 100u;
    test_ram[phys + 1u] = 0u;
    test_ram[0xB94CCu] = 0x10u;
    g_opt_focus_mode = 2;
    memory_policy_frontend_reset();

    assert(dc_memory_policy_write(phys, &value, 2u) == 1);
    assert(value == 80u);
    assert(test_ram[phys] == 100u && test_ram[phys + 1u] == 0u);
    assert(memory_policy_frontend_damage_calls() == 1);
    assert(memory_policy_frontend_damage_slot() == 0 &&
           memory_policy_frontend_damage_old() == 100 &&
           memory_policy_frontend_damage_new() == 80);
    assert(memory_policy_frontend_damage_scale() == 2.0f);
}

int main(void)
{
    test_process_init();
    set_env("DC_ARENA_MB", "1");
    set_env("DC_MOD_RAM_MB", "2");
    set_env("DC_NODE_EXTRA", "1");
    set_env("DC_DRAWLIST_WATCH", "1");
    set_env("DC_WWATCH", "");
    memset(test_ram, 0, sizeof test_ram);
    memset(test_scratchpad, 0, sizeof test_scratchpad);

    dc_memory_policy_bind();
    test_extra_memory_bounds();
    test_node_flags_and_reuse();
    test_prewrite_ammo_observation();
    test_focus_halfword_policy();

    puts("PASS: memory policy bounds, arena/mod separation, node reuse, pre-write observers, and focus halfword policy");
    return 0;
}
