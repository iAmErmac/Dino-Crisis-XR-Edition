#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dc_memory_policy.h"
#include "test_process.h"

enum { ARENA_BASE = 0x00400000u, MOD_BASE = 0x01000000u };
enum { ARENA_BYTES = 1u << 20, MOD_BYTES = 2u << 20 };
enum { NODE_BYTES = 0xACu, NODE_COUNT = 2048u, NODE_FLAGS = 0x30u };
enum { MOD_PAGE = 4096u };
enum { MODPAGE_MAGIC = 0x504D4344u, ARENA_MAGIC = 0x41584344u };

extern uint32_t dc_node_alloc_extra(void);
extern const uint8_t *memory_mod_ptr(uint32_t phys, uint32_t len);
extern uint32_t memory_mod_word(uint32_t phys);
extern void memory_modram_save(FILE *f);
extern void memory_modram_load(FILE *f);

void psx_fatal_halt(const char *reason)
{
    (void)reason;
    abort();
}

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

static void test_memory_round_trip(void)
{
    const uint32_t mod_low_phys = MOD_BASE + 0x120u;
    const uint32_t mod_sparse_phys = MOD_BASE + 3u * MOD_PAGE + 0x20u;
    const uint32_t mod_beyond_phys = MOD_BASE + 0x7000u;
    const uint32_t arena_packet_phys = ARENA_BASE + 0x180u;
    const uint32_t arena_beyond_phys = ARENA_BASE + 0x7000u;
    uint8_t *mod_low;
    uint8_t *mod_sparse;
    uint8_t *mod_beyond;
    uint8_t *arena_packet;
    uint8_t *arena_beyond;
    FILE *save;

    mod_low = dc_memory_extra_ptr(mod_low_phys, 1u);
    mod_sparse = dc_memory_extra_ptr(mod_sparse_phys, 1u);
    arena_packet = dc_memory_extra_ptr(arena_packet_phys, 4u);
    assert(mod_low && mod_sparse && arena_packet);
    *mod_low = 0x31;
    *mod_sparse = 0xA7;
    store_u32(arena_packet, 0x12345678u);

    save = tmpfile();
    assert(save);
    memory_modram_save(save);
    assert(fflush(save) == 0);

    mod_beyond = dc_memory_extra_ptr(mod_beyond_phys, 1u);
    arena_beyond = dc_memory_extra_ptr(arena_beyond_phys, 1u);
    assert(mod_beyond && arena_beyond);
    *mod_low = 0x00;
    *mod_sparse = 0x00;
    *mod_beyond = 0xDE;
    store_u32(arena_packet, 0u);
    *arena_beyond = 0xAD;

    rewind(save);
    memory_modram_load(save);
    fclose(save);

    mod_low = (uint8_t *)memory_mod_ptr(mod_low_phys, 1u);
    mod_sparse = (uint8_t *)memory_mod_ptr(mod_sparse_phys, 1u);
    mod_beyond = (uint8_t *)memory_mod_ptr(mod_beyond_phys, 1u);
    arena_packet = dc_memory_extra_ptr(arena_packet_phys, 4u);
    arena_beyond = dc_memory_extra_ptr(arena_beyond_phys, 1u);
    assert(mod_low && mod_sparse && mod_beyond && arena_packet && arena_beyond);
    assert(*mod_low == 0x31);
    assert(*mod_sparse == 0xA7);
    assert(*mod_beyond == 0x00);
    assert(load_u32(arena_packet) == 0x12345678u);
    assert(*arena_beyond == 0xAD);
}

static void test_final_byte_save_and_restore(void)
{
    const uint32_t mod_last_phys = MOD_BASE + MOD_BYTES - 1u;
    const uint32_t arena_last_phys = ARENA_BASE + ARENA_BYTES - 1u;
    uint8_t *mod_last = dc_memory_extra_ptr(mod_last_phys, 1u);
    uint8_t *arena_last = dc_memory_extra_ptr(arena_last_phys, 1u);
    uint32_t header[2];
    uint8_t page[MOD_PAGE];
    int mod_pages = 0;
    int saw_mod_last_page = 0;
    FILE *save;

    assert(mod_last && arena_last);
    *mod_last = 0x5A;
    *arena_last = 0xA5;

    save = tmpfile();
    assert(save);
    memory_modram_save(save);
    rewind(save);

    assert(fread(header, sizeof header, 1, save) == 1);
    assert(header[0] == MODPAGE_MAGIC);
    assert(header[1] == MOD_BYTES);
    assert(header[1] <= MOD_BYTES);
    for (;;) {
        uint32_t page_index;
        assert(fread(&page_index, sizeof page_index, 1, save) == 1);
        if (page_index == UINT32_MAX) break;
        assert(page_index < (MOD_BYTES / MOD_PAGE));
        assert(fread(page, sizeof page, 1, save) == 1);
        if (page_index == (MOD_BYTES / MOD_PAGE) - 1u) {
            assert(page[MOD_PAGE - 1u] == 0x5A);
            saw_mod_last_page = 1;
        }
        ++mod_pages;
    }
    assert(mod_pages > 0);
    assert(saw_mod_last_page);

    assert(fread(header, sizeof header, 1, save) == 1);
    assert(header[0] == ARENA_MAGIC);
    assert(header[1] == ARENA_BYTES);
    assert(header[1] <= ARENA_BYTES);
    assert(fseek(save, (long)header[1], SEEK_CUR) == 0);
    assert(fgetc(save) == EOF);

    *mod_last = 0;
    *arena_last = 0;
    rewind(save);
    memory_modram_load(save);
    fclose(save);

    mod_last = dc_memory_extra_ptr(mod_last_phys, 1u);
    arena_last = dc_memory_extra_ptr(arena_last_phys, 1u);
    assert(mod_last && arena_last);
    assert(*mod_last == 0x5A);
    assert(*arena_last == 0xA5);
}

int main(void)
{
    test_process_init();
    set_env("DC_ARENA_MB", "1");
    set_env("DC_MOD_RAM_MB", "2");
    set_env("DC_NODE_EXTRA", "1");

    test_memory_round_trip();
    test_final_byte_save_and_restore();
    test_extra_memory_bounds();
    test_node_flags_and_reuse();

    puts("PASS: extra-memory bounds, sparse save/load, arena packet restore, node flags, and node reuse");
    return 0;
}
