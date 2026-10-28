#include <assert.h>
#include <stdlib.h>
#include "pgxp_cells.h"
#include "dc_pgxp.h"
#include "test_process.h"
#ifdef DC_TEST_PGXP_BRIDGE
#include "pgxp_hooks.h"
#include "pgxp.h"
uint32_t g_psx_ram_mask = 0x1FFFFFu;
int psx_mod_gpu_dma_memory_contains(uint32_t address, uint32_t size)
{ (void)address; (void)size; return 0; }
int gte_geometry_correction_lookup(uint32_t raw, int32_t *x, int32_t *y, uint16_t *z)
{ (void)raw; (void)x; (void)y; (void)z; abort(); }
int gte_geometry_correction_lookup_probe(uint32_t raw, int32_t *x, int32_t *y, uint16_t *z)
{ (void)raw; (void)x; (void)y; (void)z; abort(); }
#define dc_pgxp_load(i, a, v) psx_pgxp_load(NULL, i, a, v)
#define dc_pgxp_store(i, a, v) psx_pgxp_store(NULL, i, a, v)
#define dc_pgxp_alu(i, v) psx_pgxp_alu(NULL, i, v, 0, 0)
#define dc_pgxp_cop2(i, v, a) psx_pgxp_cop2(NULL, i, v, a)
#endif

int pt_poolgeom_is_pool(uint32_t address) { (void)address; return 0; }
void pt_poolgeom_vertex(uint32_t packed, float x, float y)
{ (void)packed; (void)x; (void)y; abort(); }
int pgxp_lookup(int x, int y, float *fx, float *fy, float *w)
{ (void)x; (void)y; (void)fx; (void)fy; (void)w; return 0; }

static uint32_t mem_op(unsigned opcode, unsigned reg)
{ return (opcode << 26) | (reg << 16); }
static uint32_t cop_op(unsigned transfer, unsigned reg, unsigned gpr)
{ return (0x12u << 26) | (transfer << 21) | (gpr << 16) | (reg << 11); }

static void expect_vertex(uint32_t address, uint32_t raw, float x, float z)
{
    float fx = 0, fy = 0, w = 0;
    assert(pgxp_addr_get_vertex(address, raw, &fx, &fy, &w) == 2);
    assert(fx == x && fy == 12.5f && w == z);
}

int main(int argc, char **argv)
{
    test_process_init();
    (void)argv;
#ifdef DC_TEST_PGXP_BRIDGE
    if (argc > 1) { pgxp_set_enabled(1); assert(pgxp_active()); }
#else
    (void)argc;
#endif
    pgxp_addr_boot();
    assert(g_pgxp_addr_on);
    g_px_reseed = 0;
    const uint32_t raw = 0x000C000Au, first = 0x1000, second = 0x2000;
    dc_pgxp_cop2(mem_op(0x32, 0), 0x1234, 0x9000);
    pgxp_addr_rtps_push(10.25f, 12.5f, 42, raw, 1);
    dc_pgxp_cop2(mem_op(0x3A, 14), raw, first);
    expect_vertex(first, raw, 10.25f, 42);
    assert(pgxp_addr_vertex_src(first, raw) == 0x9000);
    dc_pgxp_load(mem_op(0x23, 2), first, raw);
    dc_pgxp_alu((2u << 21) | (3u << 11) | 0x21u, raw);
    dc_pgxp_store(mem_op(0x2B, 3), second, raw);
    expect_vertex(second, raw, 10.25f, 42);
    assert(pgxp_addr_vertex_src(second, raw) == 0x9000);
#ifdef DC_TEST_PGXP_BRIDGE
    psx_pgxp_forget_register(3);
#else
    dc_pgxp_forget_register(3);
#endif
    assert(g_px_reg[3].bits == 0);
    dc_pgxp_store(mem_op(0x2B, 3), 0x2800, raw);
    assert(!px_is_live(0x2800));
    dc_pgxp_load(mem_op(0x23, 4), 0x3000, 0x1234);
    dc_pgxp_alu((4u << 21) | (5u << 11) | 0x25u, 0x1234);
    dc_pgxp_cop2(cop_op(4, 0, 5), 0x1234, 0);
    pgxp_addr_rtps_push(10.75f, 12.5f, 84, raw, 1);
    dc_pgxp_cop2(mem_op(0x3A, 14), raw, 0x4000);
    expect_vertex(first, raw, 10.25f, 42);
    expect_vertex(0x4000, raw, 10.75f, 84);
    assert(pgxp_addr_vertex_src(0x4000, raw) == 0x3000);
    dc_pgxp_cop2(cop_op(0, 14, 6), raw, 0);
    dc_pgxp_cop2(cop_op(4, 15, 6), raw, 0);
    dc_pgxp_cop2(mem_op(0x3A, 14), raw, 0x5000);
    expect_vertex(0x5000, raw, 10.75f, 84);
    dc_pgxp_cop2(mem_op(0x32, 15), raw, 0x4000);
    dc_pgxp_cop2(mem_op(0x3A, 14), raw, 0x7000);
    expect_vertex(0x7000, raw, 10.75f, 84);
    dc_pgxp_load(mem_op(0x25, 7), 0x4000, (uint16_t)raw);
    assert((g_px_reg[7].bits & PX_XOK) != 0);
    dc_pgxp_store(mem_op(0x29, 7), 0x8000, (uint16_t)raw);
    assert(px_is_live(0x8000));
    dc_pgxp_load(mem_op(0x24, 7), 0x4000, raw & 0xFF);
    assert(g_px_reg[7].bits == 0);
    dc_pgxp_store(mem_op(0x2A, 0), 0x7001, raw);
    assert(!px_is_live(0x7000));
    dc_pgxp_store(mem_op(0x28, 0), second + 1, 0);
    assert(!px_is_live(second));
    dc_pgxp_load(mem_op(0x23, 2), first, raw + 1);
    assert(g_px_reg[2].bits == 0 && !px_is_live(first));
    dc_pgxp_alu((9u << 26) | (5u << 21) | (5u << 16) | 1u, 0x1235);
    dc_pgxp_cop2(cop_op(4, 0, 5), 0x1235, 0);
    pgxp_addr_rtps_push(10.25f, 12.5f, 42, raw, 1);
    dc_pgxp_cop2(mem_op(0x3A, 14), raw, 0x6000);
    assert(pgxp_addr_vertex_src(0x6000, raw) == 0);
    pgxp_addr_reset();
    assert(!px_is_live(0x4000) && g_px_reg[6].bits == 0);
    pgxp_addr_rtps_push(10.25f, 12.5f, 42, raw, 1);
    dc_pgxp_cop2(mem_op(0x3A, 14), raw, first);
    assert(pgxp_addr_vertex_src(first, raw) == 0);
    free(g_px_cell); free(g_px_live);
    return 0;
}
