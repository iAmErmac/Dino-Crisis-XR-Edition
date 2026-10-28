#ifndef PSXRECOMP_PGXP_ADDR_H
#define PSXRECOMP_PGXP_ADDR_H

#include "pgxp_cells.h"
#include "cpu_state.h"
#include "psx_fastmem.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Fast guest writes (09.09.2026): main RAM pages nobody watches are written here directly; everything watched and
 * everything outside main RAM goes the full way (memory.c). They live here, not in psx_fastmem.h, because the fast
 * path must empty the PGXP cells exactly like the full one does. */
PSX_HOT_INLINE void psx_ww_f(uint32_t addr, uint32_t val) {
    const uint32_t phys = addr & 0x1FFFFFFFu;
    if (!psx_fm_wr_ok(phys)) { psx_write_word(addr, val); return; }
    if (g_pgxp_addr_on) pgxp_addr_inval_word(phys);
    psx_fm_mark_dirty(phys);
    memcpy(g_psx_ram + phys, &val, 4);
}
PSX_HOT_INLINE void psx_wh_f(uint32_t addr, uint16_t val) {
    const uint32_t phys = addr & 0x1FFFFFFFu;
    if (!psx_fm_wr_ok(phys)) { psx_write_half(addr, val); return; }
    if (g_pgxp_addr_on) pgxp_addr_inval_half(phys, val);
    psx_fm_mark_dirty(phys);
    memcpy(g_psx_ram + phys, &val, 2);
}
PSX_HOT_INLINE void psx_wb_f(uint32_t addr, uint8_t val) {
    const uint32_t phys = addr & 0x1FFFFFFFu;
    if (!psx_fm_wr_ok(phys)) { psx_write_byte(addr, val); return; }
    if (g_pgxp_addr_on) pgxp_addr_inval_word(phys);
    psx_fm_mark_dirty(phys);
    g_psx_ram[phys] = val;
}

/* ---- wrappers used by the transformed generated code ------------------------------------------------------
 * The guest access itself is done first and exactly as before; only then the cells follow. A store from a
 * register without a cell costs nothing more than the emptying the write already did; a load from a word
 * without a cell only empties the destination register's cell. */

/* Direct guest reads (09.09.2026): the memory pointers in CPUState always point at psx_read_* / psx_write_*;
 * main RAM is read here without the indirect call (all watchers sit on WRITES). */
extern uint8_t *const g_psx_ram;
extern uint32_t psx_read_word(uint32_t addr);
extern uint16_t psx_read_half(uint32_t addr);
extern void     psx_write_word(uint32_t addr, uint32_t value);
extern void     psx_write_half(uint32_t addr, uint16_t value);
#define PX_RAM_BYTES 0x200000u

PSX_HOT_INLINE uint32_t pgxp_rd_word(uint32_t addr) {
    const uint32_t phys = addr & 0x1FFFFFFFu;
    if (phys >= PX_RAM_BYTES) return psx_read_word(addr);
    uint32_t v; memcpy(&v, g_psx_ram + phys, 4); return v;
}
PSX_HOT_INLINE uint16_t pgxp_rd_half(uint32_t addr) {
    const uint32_t phys = addr & 0x1FFFFFFFu;
    if (phys >= PX_RAM_BYTES) return psx_read_half(addr);
    uint16_t v; memcpy(&v, g_psx_ram + phys, 2); return v;
}

/* The guest registers are locals of the generated function (code_generator.cpp): the value comes as an argument. */
static inline void pgxp_sw_v(CPUState* cpu, uint32_t addr, int r, uint32_t v) {
    (void)cpu;
    psx_ww_f(addr, v);
    if (!g_pgxp_addr_on) return;
    if (px_reg_holds(r, v)) { px_store_word_from_reg(addr, r); return; }
    /* 16.08.2026 ("polygons merge / vanish with distance"): the room renderer (func_80064268 family) repacks
     * projected vertices through plain arithmetic before storing them into the display-list arena, so nothing
     * follows them there and the raster falls back to whole pixels - distant triangles collapse. An untracked
     * store into that arena that looks like a packed screen vertex gets its cell back from the value-keyed GTE
     * cache; a collided entry still lies in the same whole pixel, never worse than the integer fallback. */
    if (g_px_reseed && ((addr & 0x1FFFFFFu) - 0x150000u) < 0x80000u && v != 0) {
        const int32_t sx = (int16_t)v, sy = (int16_t)(v >> 16);
        if (sx > -1024 && sx < 1024 && sy > -512 && sy < 512) px_rebuild_from_cache(addr, v);
    }
}

static inline uint32_t pgxp_lw(CPUState* cpu, uint32_t addr, int r) {
    (void)cpu;
    const uint32_t v = pgxp_rd_word(addr);
    if (!g_pgxp_addr_on) return v;
    if (px_is_live(addr)) { px_load_word_to_reg(addr, r, v); return v; }
    g_px_reg[r].bits = 0;
    /* A pool word whose cell was emptied by an untracked store: the precise position is gone, but the vertex is
     * still a character vertex - keep that alone, tied to the word, so the polygon does not flip between actor
     * and world for a frame (the class drives the debug colour and the back-face depth bias, never positions). */
    if (r != 0 && ((addr & 0x1FFFFFFu) - 0xC6800u) < 0x2000u) {
        g_px_reg[r].bits = PX_ACTOR;
        g_px_reg[r].raw = v;
    }
    return v;
}

static inline void pgxp_sh_v(CPUState* cpu, uint32_t addr, int r, uint32_t v) {
    (void)cpu;
    psx_wh_f(addr, (uint16_t)v);
    if (g_pgxp_addr_on && px_reg_holds(r, v)) px_store_half_from_reg(addr, r, v);
}

static inline uint32_t pgxp_lhu(CPUState* cpu, uint32_t addr, int r) {
    (void)cpu;
    const uint32_t v = (uint32_t)pgxp_rd_half(addr);
    if (g_pgxp_addr_on) {
        if (px_is_live(addr)) px_load_half_to_reg(addr, r, v, 0);
        else g_px_reg[r].bits = 0;
    }
    return v;
}

static inline uint32_t pgxp_lh(CPUState* cpu, uint32_t addr, int r) {
    (void)cpu;
    const uint32_t v = (uint32_t)(int32_t)(int16_t)pgxp_rd_half(addr);
    if (g_pgxp_addr_on) {
        if (px_is_live(addr)) px_load_half_to_reg(addr, r, v, 1);
        else g_px_reg[r].bits = 0;
    }
    return v;
}

static inline uint32_t pgxp_move_v(CPUState* cpu, int rd, int rs, uint32_t v) {
    (void)cpu;
    if (!g_pgxp_addr_on || rd == 0) return v;
    if (rs != 0 && px_reg_holds(rs, v)) g_px_reg[rd] = g_px_reg[rs];
    else g_px_reg[rd].bits = 0;
    return v;
}

static inline uint32_t pgxp_mfc2_sxy(CPUState* cpu, int r, int reg) {
    const uint32_t v = cpu->gte_data[reg];
    if (g_pgxp_addr_on) px_gte_to_reg(r, reg, v);
    return v;
}

static inline void pgxp_mtc2_sxy_v(CPUState* cpu, int reg, int r, uint32_t v) {
    cpu->gte_data[reg] = v;
    if (g_pgxp_addr_on) px_reg_to_gte(reg, r, v);
}

static inline void pgxp_swc2_sxy(CPUState* cpu, uint32_t addr, int reg) {
    const uint32_t v = cpu->gte_data[reg];
    psx_ww_f(addr, v);
    if (g_pgxp_addr_on) px_gte_to_mem(addr, reg, v);
}

static inline void pgxp_lwc2_sxy(CPUState* cpu, uint32_t addr, int reg) {
    const uint32_t v = pgxp_rd_word(addr);
    cpu->gte_data[reg] = v;
    if (g_pgxp_addr_on) px_mem_to_gte(addr, reg, v);
}

/* Older three-argument forms (the value from CPUState) - generated code from before the registers became locals. */
static inline void pgxp_sw(CPUState* cpu, uint32_t addr, int r) { pgxp_sw_v(cpu, addr, r, cpu->gpr[r]); }
static inline void pgxp_sh(CPUState* cpu, uint32_t addr, int r) { pgxp_sh_v(cpu, addr, r, cpu->gpr[r]); }
static inline uint32_t pgxp_move(CPUState* cpu, int rd, int rs) { return pgxp_move_v(cpu, rd, rs, cpu->gpr[rs]); }
static inline void pgxp_mtc2_sxy(CPUState* cpu, int reg, int r) { pgxp_mtc2_sxy_v(cpu, reg, r, cpu->gpr[r]); }

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_PGXP_ADDR_H */
