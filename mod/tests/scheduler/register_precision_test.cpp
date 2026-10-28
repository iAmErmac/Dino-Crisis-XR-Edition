#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>

#include "dc_register_precision.h"
#include "dc_pgxp.h"
#include "pgxp.h"
#include "pgxp_cells.h"
#include "pgxp_hooks.h"
#include "test_process.h"

extern "C" {
uint32_t g_psx_ram_size = 0x00200000u;
uint32_t g_psx_ram_mask = 0x001FFFFFu;
void pgxp_addr_reset(void);

int psx_mod_gpu_dma_memory_contains(uint32_t address, uint32_t size)
{
    (void)address;
    (void)size;
    return 0;
}

int gte_geometry_correction_lookup(uint32_t raw, int32_t *x, int32_t *y, uint16_t *z)
{
    (void)raw;
    (void)x;
    (void)y;
    (void)z;
    abort();
}

int gte_geometry_correction_lookup_probe(uint32_t raw, int32_t *x, int32_t *y, uint16_t *z)
{
    (void)raw;
    (void)x;
    (void)y;
    (void)z;
    abort();
}

int pt_poolgeom_is_pool(uint32_t address)
{
    (void)address;
    return 0;
}

void pt_poolgeom_vertex(uint32_t packed, float x, float y)
{
    (void)packed;
    (void)x;
    (void)y;
    abort();
}

int pgxp_lookup(int x, int y, float *fx, float *fy, float *w)
{
    (void)x;
    (void)y;
    (void)fx;
    (void)fy;
    (void)w;
    return 0;
}
}

static uint32_t cop2(unsigned transfer, unsigned reg, unsigned gpr)
{
    return (0x12u << 26) | (transfer << 21) | (gpr << 16) | (reg << 11);
}

static uint32_t memory_op(unsigned opcode, unsigned reg)
{
    return (opcode << 26) | (reg << 16);
}

static uint32_t mflo(unsigned rd)
{
    return (rd << 11) | 0x12u;
}

static uint32_t mult(unsigned rs, unsigned rt)
{
    return (rs << 21) | (rt << 16) | 0x18u;
}

static void expect_generic_shadow(int space, unsigned key, uint32_t value,
                                  int32_t x16, int32_t y16, uint16_t z)
{
    int live = 0;
    uint32_t got_value = 0, flags = 0;
    int32_t got_x = 0, got_y = 0;
    uint16_t got_z = 0;
    assert(pgxp_debug_shadow(space, key, &live, &got_value, &flags,
                             &got_x, &got_y, &got_z));
    assert(live);
    assert(got_value == value);
    assert((flags & 0x7u) == (z ? 0x7u : 0x3u));
    assert(got_x == x16 && got_y == y16 && got_z == z);
}

static void expect_generic_dead(int space, unsigned key)
{
    int live = 1;
    uint32_t value = 0, flags = 0;
    int32_t x16 = 0, y16 = 0;
    uint16_t z = 0;
    assert(pgxp_debug_shadow(space, key, &live, &value, &flags,
                             &x16, &y16, &z));
    assert(!live);
}

static void seed_projection(uint32_t raw, uint32_t origin,
                            int32_t x16, int32_t y16, uint16_t depth)
{
    dc_pgxp_cop2(memory_op(0x32u, 0u), raw, origin);
    dc_pgxp_project(0, x16, y16, depth, raw, 1);
    pgxp_gte_push_sxy(x16, y16, depth, raw);
}

static void test_projection_round_trip(void)
{
    const uint32_t raw = 0x0014000Au;
    const uint32_t origin = 0x1000u;
    const uint32_t source = 0x2000u;
    const uint32_t destination = 0x3000u;
    const int32_t x16 = 10 * 65536 + 16384;
    const int32_t y16 = 20 * 65536 + 32768;
    const uint16_t depth = 42;
    uint32_t words[34] = {};
    PsxRegisterPrecision generic_state = {};
    DcAddressPrecision address_state = {};

    seed_projection(raw, origin, x16, y16, depth);
    psx_pgxp_cop2(NULL, memory_op(0x3Au, 14u), raw, source);
    psx_pgxp_load(NULL, memory_op(0x23u, 5u), source, raw);
    words[5] = raw;

    expect_generic_shadow(1, 5, raw, x16, y16, depth);
    expect_generic_shadow(2, 14, raw, x16, y16, depth);
    float fx = 0.0f, fy = 0.0f, fw = 0.0f;
    assert(pgxp_addr_get_vertex(source, raw, &fx, &fy, &fw) == 2);
    assert(fx == (float)x16 / 65536.0f);
    assert(fy == (float)y16 / 65536.0f);
    assert(fw == (float)depth);
    assert(pgxp_addr_vertex_src(source, raw) == origin);

    psx_pgxp_capture_registers(words, &generic_state);
    dc_pgxp_capture_registers(words, &address_state);
    assert(generic_state.generation != 0);
    assert(generic_state.reg[5].live == 1);
    assert(generic_state.reg[5].value == raw);
    assert(generic_state.reg[5].x16 == x16);
    assert(generic_state.reg[5].y16 == y16);
    assert(generic_state.reg[5].z == depth);
    assert(address_state.generation != 0);
    assert(address_state.reg[5].raw == raw);
    assert((address_state.reg[5].bits & (PX_XY | PX_ZOK)) == (PX_XY | PX_ZOK));
    assert(address_state.reg[5].origin == origin);

    psx_pgxp_forget_register(5);
    assert(!g_px_reg[5].bits);
    expect_generic_dead(1, 5);
    psx_pgxp_restore_registers(words, &generic_state);
    dc_pgxp_restore_registers(words, &address_state);
    expect_generic_shadow(1, 5, raw, x16, y16, depth);
    assert((g_px_reg[5].bits & (PX_XY | PX_ZOK)) == (PX_XY | PX_ZOK));

    psx_pgxp_store(NULL, memory_op(0x2Bu, 5u), destination, raw);
    psx_pgxp_load(NULL, memory_op(0x23u, 6u), destination, raw);
    words[6] = raw;
    expect_generic_shadow(1, 6, raw, x16, y16, depth);
    assert(pgxp_addr_get_vertex(destination, raw, &fx, &fy, &fw) == 2);
    assert(fx == (float)x16 / 65536.0f);
    assert(fy == (float)y16 / 65536.0f);
    assert(fw == (float)depth);
    assert(pgxp_addr_vertex_src(destination, raw) == origin);
    expect_generic_shadow(2, 14, raw, x16, y16, depth);
    assert(pgxp_addr_vertex_src(source, raw) == origin);
}

static void test_word_mismatch_and_invalid_receipt(void)
{
    const uint32_t raw = 0x0014000Au;
    uint32_t words[34] = {};
    PsxRegisterPrecision captured = {};
    PsxRegisterPrecision invalid = {};
    DcAddressPrecision address_state = {};

    words[6] = raw;
    psx_pgxp_capture_registers(words, &captured);
    dc_pgxp_capture_registers(words, &address_state);
    assert(captured.reg[6].live == 1);

    psx_pgxp_forget_register(6);
    words[6] = raw + 1u;
    assert(psx_pgxp_restore_registers(words, &captured) == 1);
    dc_pgxp_restore_registers(words, &address_state);
    expect_generic_dead(1, 6);
    assert(g_px_reg[6].bits == 0);

    words[6] = raw;
    psx_pgxp_restore_registers(words, &captured);
    dc_pgxp_restore_registers(words, &address_state);
    expect_generic_shadow(1, 6, raw, captured.reg[6].x16,
                          captured.reg[6].y16, captured.reg[6].z);

    invalid = captured;
    invalid.reg[6].flags = (uint16_t)(invalid.reg[6].flags | 0x8000u);
    assert(psx_pgxp_restore_registers(words, &invalid) == 0);
    expect_generic_shadow(1, 6, raw, captured.reg[6].x16,
                          captured.reg[6].y16, captured.reg[6].z);
}

static void test_scalar_round_trip(void)
{
    const uint32_t scalar_word = 10u;
    const uint32_t scalar_result = 30u;
    const int32_t scalar_x16 = 10 * 65536 + 16384;
    const int32_t result_x16 = 30 * 65536 + 49152;
    const uint32_t destination = 0x6000u;
    uint32_t words[34] = {};
    PsxRegisterPrecision state = {};

    pgxp_gte_push_sxy(scalar_x16, 0, 0, scalar_word);
    psx_pgxp_cop2(NULL, cop2(0, 14, 5), scalar_word, 0);
    psx_pgxp_muldiv(NULL, mult(5, 6), 0, scalar_result,
                    scalar_word, 3u);
    psx_pgxp_alu(NULL, mflo(7), scalar_result, 0, 0);
    words[7] = scalar_result;
    words[33] = scalar_result;
    psx_pgxp_capture_registers(words, &state);
    assert(state.reg[7].scalar_live == 1);
    assert(fabs(state.reg[7].scalar - 30.75) < 0.000001);

    psx_pgxp_forget_register(7);
    psx_pgxp_restore_registers(words, &state);
    psx_pgxp_store(NULL, memory_op(0x2Bu, 7u), destination, scalar_result);
    psx_pgxp_load(NULL, memory_op(0x23u, 8u), destination, scalar_result);
    expect_generic_shadow(1, 8, scalar_result, result_x16, 0, 0);
}

static void test_generation_invalidation(void)
{
    const uint32_t raw = 0x0014000Au;
    const uint32_t address = 0x3000u;
    uint32_t words[34] = {};
    PsxRegisterPrecision generic_state = {};
    DcAddressPrecision address_state = {};

    words[6] = raw;
    psx_pgxp_capture_registers(words, &generic_state);
    dc_pgxp_capture_registers(words, &address_state);
    float fx = 0.0f, fy = 0.0f, fw = 0.0f;
    assert(pgxp_addr_get_vertex(address, raw, &fx, &fy, &fw) == 2);

    pgxp_invalidate_all();
    pgxp_addr_reset();
    assert(psx_pgxp_restore_registers(words, &generic_state) == 1);
    dc_pgxp_restore_registers(words, &address_state);
    expect_generic_dead(1, 6);
    expect_generic_dead(0, address);
    assert(g_px_reg[6].bits == 0);
    assert(pgxp_addr_get_vertex(address, raw, &fx, &fy, &fw) == 0);
}

static void test_disabled_generic(void)
{
    uint32_t words[34] = {};
    PsxRegisterPrecision state = {};

    pgxp_set_cpu_mode(0);
    pgxp_set_enabled(0);
    assert(!pgxp_active());
    psx_pgxp_capture_registers(words, &state);
    for (unsigned i = 1; i < 34; ++i) {
        assert(state.reg[i].live == 0);
        assert(state.reg[i].scalar_live == 0);
    }
}

int main()
{
    test_process_init();
    pgxp_addr_boot();
    assert(g_pgxp_addr_on);
    pgxp_set_enabled(1);
    pgxp_set_cpu_mode(1);
    assert(pgxp_active());
    g_px_reseed = 0;

    test_projection_round_trip();
    test_word_mismatch_and_invalid_receipt();
    test_scalar_round_trip();
    test_generation_invalidation();
    test_disabled_generic();
    free(g_px_cell);
    free(g_px_live);
    return 0;
}
