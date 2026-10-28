#include "dc_memory_policy.h"
#include "dc_memory_snapshot.h"
#include "pst_wire.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define PSX_HOT_INLINE static inline
extern void psx_fatal_halt(const char *reason);
static _Noreturn void policy_fail(const char *reason) { psx_fatal_halt(reason); abort(); }

/* GPU links address packets through a 24-bit field. */
#define PSX_EXT_BASE  0x00400000u
static uint8_t *s_ext_ram = NULL;
static uint32_t s_ext_size = 0;
uint32_t psx_ext_base(void) { return PSX_EXT_BASE; }
uint32_t psx_ext_size(void) {
    if (!s_ext_size) {
        const char *e = getenv("DC_ARENA_MB");

        int mb = e ? atoi(e) : 8;
        if (mb < 1) mb = 1;
        if (mb > 12) mb = 12;
        s_ext_size = (uint32_t)mb * 1024u * 1024u;
    }
    return s_ext_size;
}

#define PSX_MOD_BASE  0x01000000u
static uint8_t *s_mod_ram = NULL;
static uint32_t s_mod_size = 0xFFFFFFFFu, s_mod_hwm = 0;
static uint32_t psx_mod_size(void) {
    if (s_mod_size == 0xFFFFFFFFu) {
        const char *e = getenv("DC_MOD_RAM_MB");
        int mb = e ? atoi(e) : 32;
        if (mb < 0) mb = 0;
        if (mb > 256) mb = 256;
        s_mod_size = (uint32_t)mb * 1024u * 1024u;
    }
    return s_mod_size;
}

PSX_HOT_INLINE uint8_t *mod_at(uint32_t phys) {
    const uint32_t off = phys - PSX_MOD_BASE;
    if (off >= (s_mod_size != 0xFFFFFFFFu ? s_mod_size : psx_mod_size())) return NULL;
    if (!s_mod_ram) {
        s_mod_ram = (uint8_t*)calloc(1, (size_t)psx_mod_size() + 16u);
        if (!s_mod_ram) return NULL;
        fprintf(stderr, "modram: %u MB for mods mapped at %08X%c", psx_mod_size() / (1024u*1024u), 0x80000000u | PSX_MOD_BASE, 10);
    }
    if (off + 4u > s_mod_hwm) s_mod_hwm = off + 4u;
    return s_mod_ram + off;
}
static uint32_t s_ext_hwm = 0;
PSX_HOT_INLINE uint8_t *ext_at(uint32_t phys) {
    if (phys >= PSX_MOD_BASE) return mod_at(phys);
    if (phys < PSX_EXT_BASE) return NULL;
    uint32_t off = phys - PSX_EXT_BASE;
    if (off >= (s_ext_size ? s_ext_size : psx_ext_size())) return NULL;
    if (!s_ext_ram) {
        s_ext_ram = (uint8_t*)calloc(1, psx_ext_size() + 16u);
        if (!s_ext_ram) return NULL;
        fprintf(stderr, "arena: %u MB of packet memory mapped at %08X%c",
                psx_ext_size() / (1024u*1024u), 0x80000000u | PSX_EXT_BASE, 10);
    }
    if (off + 4u > s_ext_hwm) s_ext_hwm = off + 4u;
    return s_ext_ram + off;
}

#define MODRAM_MAGIC 0x584D4344u
#define MODPAGE_MAGIC 0x504D4344u
#define ARENA_MAGIC  0x41584344u
#define MOD_PAGE 4096u
void memory_modram_save(FILE *f) {
    if (s_mod_ram && s_mod_hwm) {

        const uint32_t hdr[2] = { MODPAGE_MAGIC, s_mod_hwm };
        fwrite(hdr, sizeof hdr, 1, f);
        const uint32_t np = (s_mod_hwm + MOD_PAGE - 1u) / MOD_PAGE;
        for (uint32_t pg = 0; pg < np; pg++) {
            const uint8_t *p = s_mod_ram + (size_t)pg * MOD_PAGE;
            int any = 0;
            for (uint32_t k = 0; k < MOD_PAGE && !any; k += 8) { uint64_t w; memcpy(&w, p + k, 8); any = w != 0; }
            if (!any) continue;
            fwrite(&pg, 4, 1, f);
            fwrite(p, 1, MOD_PAGE, f);
        }
        const uint32_t end = 0xFFFFFFFFu;
        fwrite(&end, 4, 1, f);
    }
    if (s_ext_ram && s_ext_hwm) {
        const uint32_t hdr[2] = { ARENA_MAGIC, s_ext_hwm };
        fwrite(hdr, sizeof hdr, 1, f);
        fwrite(s_ext_ram, 1, s_ext_hwm, f);
    }
}
void memory_modram_load(FILE *f) {
    int mod_seen = 0;
    for (int k = 0; k < 4; k++) {
        uint32_t hdr[2];
        if (fread(hdr, sizeof hdr, 1, f) != 1 || hdr[1] == 0) break;
        if (hdr[0] == MODRAM_MAGIC && hdr[1] <= psx_mod_size()) {
            const uint32_t old = s_mod_hwm;
            if (!mod_at(PSX_MOD_BASE + hdr[1] - 4u)) break;
            if (fread(s_mod_ram, 1, hdr[1], f) != hdr[1]) break;
            if (old > hdr[1]) memset(s_mod_ram + hdr[1], 0, old - hdr[1]);
            s_mod_hwm = hdr[1];
            mod_seen = 1;
        } else if (hdr[0] == MODPAGE_MAGIC && hdr[1] <= psx_mod_size()) {
            const uint32_t old = s_mod_hwm;
            if (!mod_at(PSX_MOD_BASE + hdr[1] - 4u)) break;
            memset(s_mod_ram, 0, old > hdr[1] ? old : hdr[1]);
            s_mod_hwm = hdr[1];
            mod_seen = 1;
            const uint32_t np = (hdr[1] + MOD_PAGE - 1u) / MOD_PAGE;
            for (;;) {
                uint32_t pg;
                if (fread(&pg, 4, 1, f) != 1 || pg == 0xFFFFFFFFu || pg >= np) break;
                if (fread(s_mod_ram + (size_t)pg * MOD_PAGE, 1, MOD_PAGE, f) != MOD_PAGE) break;
            }
        } else if (hdr[0] == ARENA_MAGIC && hdr[1] <= psx_ext_size()) {
            if (!ext_at(PSX_EXT_BASE + hdr[1] - 4u)) break;
            if (fread(s_ext_ram, 1, hdr[1], f) != hdr[1]) break;
        } else break;
    }

    if (!mod_seen && s_mod_ram && s_mod_hwm) memset(s_mod_ram, 0, s_mod_hwm);
}

static uint32_t s_node_next;

uint32_t dc_node_alloc_extra(void) {
    static int en = -1;
    if (en < 0) { const char *e = getenv("DC_NODE_EXTRA"); en = (e && *e == '0') ? 0 : 1; }
    const uint32_t size = psx_mod_size();
    if (!en || size < 0x200000u) return 0;
    const uint32_t sz = 0xACu, n = 2048u;
    const uint32_t base = PSX_MOD_BASE + size - 0x100000u;
    for (uint32_t k = 0; k < n; k++) {
        const uint32_t i = (s_node_next + k) % n;
        uint8_t *p = mod_at(base + i * sz);
        if (!p || !mod_at(base + i * sz + sz - 4u)) return 0;
        uint32_t fl;
        memcpy(&fl, p + 0x30, 4);
        if (fl & 1u) continue;
        memset(p, 0, sz);
        fl = 3u;
        memcpy(p + 0x30, &fl, 4);
        s_node_next = (i + 1u) % n;
        static int said = 0;
        if (!said) {
            said = 1;
            fprintf(stderr, "nodes: the game's 100 skeleton nodes are used up - more from the memory for mods at %08X%c",
                    0x80000000u | base, 10);
        }
        return 0x80000000u | (base + i * sz);
    }
    return 0;
}

const uint8_t *memory_mod_ptr(uint32_t phys, uint32_t len) {
    if (!s_mod_ram || phys < PSX_MOD_BASE) return NULL;
    const uint32_t off = phys - PSX_MOD_BASE;
    if (off + len > psx_mod_size() || off + len < off) return NULL;
    return s_mod_ram + off;
}

#define FK_LO 0x02E00000u
#define FK_HI 0x02F00000u
int memory_fk_code(uint32_t phys) {
    static int en = -1;
    if (en < 0) { const char *e = getenv("DC_FOREIGN_KINDS"); en = (e && *e == '0') ? 0 : 1; }
    if (!en || phys < FK_LO || phys >= FK_HI || !s_mod_ram) return 0;
    return phys - PSX_MOD_BASE + 4u <= psx_mod_size();
}
uint32_t memory_mod_word(uint32_t phys) {
    const uint8_t *p = memory_mod_ptr(phys, 4);
    uint32_t w = 0;
    if (p) memcpy(&w, p, 4);
    return w;
}


int dc_memory_extra_span(uint32_t phys, uint32_t width) {
    if (!width || phys < PSX_EXT_BASE) return 0;
    uint32_t base = phys < PSX_MOD_BASE ? PSX_EXT_BASE : PSX_MOD_BASE;
    uint32_t size = phys < PSX_MOD_BASE ? psx_ext_size() : psx_mod_size();
    uint32_t offset = phys - base;
    if (offset >= size || width > size - offset) return 0;
    return 1;
}
uint8_t *dc_memory_extra_ptr(uint32_t phys, uint32_t width) {
    if (!dc_memory_extra_span(phys, width)) return NULL;
    uint32_t base = phys < PSX_MOD_BASE ? PSX_EXT_BASE : PSX_MOD_BASE;
    uint32_t offset = phys - base;
    uint32_t *used = phys < PSX_MOD_BASE ? &s_ext_hwm : &s_mod_hwm;
    uint32_t before = *used;
    uint8_t *result = ext_at(phys);
    if (!result) policy_fail("memory: extra storage allocation failed");
    *used = before > offset + width ? before : offset + width;
    return result;
}

typedef struct DcMemoryWire {
    uint32_t version, ext_capacity, mod_capacity, ext_used, mod_used, node_next;
    uint32_t ext_pages, mod_pages;
} DcMemoryWire;

static int dc_memory_page_used(const uint8_t *p, size_t size)
{
    for (size_t i = 0; i < size; ++i) if (p[i]) return 1;
    return 0;
}

static uint32_t dc_memory_page_count(const uint8_t *ram, uint32_t used)
{
    uint32_t count = 0;
    for (uint32_t offset = 0; offset < used; offset += MOD_PAGE) {
        uint32_t size = used - offset < MOD_PAGE ? used - offset : MOD_PAGE;
        if (dc_memory_page_used(ram + offset, size)) ++count;
    }
    return count;
}

static int dc_memory_header(PstW *w, PstR *r, DcMemoryWire *h)
{
#define FIELD(x) do { if (r ? !pst_r_u32(r, &h->x) : !pst_w_u32(w, h->x)) return 0; } while (0)
    FIELD(version); FIELD(ext_capacity); FIELD(mod_capacity);
    FIELD(ext_used); FIELD(mod_used); FIELD(node_next); FIELD(ext_pages); FIELD(mod_pages);
#undef FIELD
    return 1;
}

static int dc_memory_pages_write(PstW *w, const uint8_t *ram, uint32_t used)
{
    for (uint32_t offset = 0; offset < used; offset += MOD_PAGE) {
        uint32_t size = used - offset < MOD_PAGE ? used - offset : MOD_PAGE;
        if (!dc_memory_page_used(ram + offset, size)) continue;
        if (!pst_w_u32(w, offset / MOD_PAGE) || !pst_w_bytes(w, ram + offset, size)) return 0;
    }
    return 1;
}

static int dc_memory_capture(DcMemoryWire *h, PstW *w)
{
    if ((s_ext_hwm && !s_ext_ram) || (s_mod_hwm && !s_mod_ram) ||
        s_ext_hwm > psx_ext_size() || s_mod_hwm > psx_mod_size() || s_node_next >= 2048) return 0;
    h->version = 1; h->ext_capacity = psx_ext_size(); h->mod_capacity = psx_mod_size();
    h->ext_used = s_ext_hwm; h->mod_used = s_mod_hwm; h->node_next = s_node_next;
    h->ext_pages = dc_memory_page_count(s_ext_ram, s_ext_hwm);
    h->mod_pages = dc_memory_page_count(s_mod_ram, s_mod_hwm);
    return dc_memory_header(w, NULL, h) && dc_memory_pages_write(w, s_ext_ram, h->ext_used) &&
        dc_memory_pages_write(w, s_mod_ram, h->mod_used);
}

size_t dc_memory_snapshot_bytes(void)
{
    DcMemoryWire h; PstW w; pst_w_init(&w, NULL, 0);
    return dc_memory_capture(&h, &w) ? w.written : 0;
}

int dc_memory_snapshot_write(uint8_t *data, size_t size)
{
    DcMemoryWire h; PstW w;
    size_t required = dc_memory_snapshot_bytes();
    if (!data || !required || size != required) return 0;
    pst_w_init(&w, data, size);
    return dc_memory_capture(&h, &w) && w.written == size;
}

static int dc_memory_pages_read(PstR *r, uint8_t *ram, uint32_t used, uint32_t count)
{
    uint32_t previous = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t page;
        if (!pst_r_u32(r, &page) || page >= (used + MOD_PAGE - 1) / MOD_PAGE ||
            (i && page <= previous)) return 0;
        uint32_t offset = page * MOD_PAGE;
        uint32_t size = used - offset < MOD_PAGE ? used - offset : MOD_PAGE;
        if (!pst_r_bytes(r, ram ? ram + offset : NULL, size)) return 0;
        previous = page;
    }
    return 1;
}

static int dc_memory_decode(const uint8_t *data, size_t size, DcMemoryWire *h, PstR *pages)
{
    PstR r;
    if (!data || size < 32) return 0;
    pst_r_init(&r, data, size);
    if (!dc_memory_header(NULL, &r, h) || h->version != 1 ||
        h->ext_capacity != psx_ext_size() || h->mod_capacity != psx_mod_size() ||
        h->ext_used > h->ext_capacity || h->mod_used > h->mod_capacity || h->node_next >= 2048 ||
        h->ext_pages > (h->ext_used + MOD_PAGE - 1) / MOD_PAGE ||
        h->mod_pages > (h->mod_used + MOD_PAGE - 1) / MOD_PAGE) return 0;
    *pages = r;
    return dc_memory_pages_read(&r, NULL, h->ext_used, h->ext_pages) &&
        dc_memory_pages_read(&r, NULL, h->mod_used, h->mod_pages) && r.p == r.end;
}

int dc_memory_snapshot_validate(const uint8_t *data, size_t size)
{
    DcMemoryWire h; PstR pages;
    return dc_memory_decode(data, size, &h, &pages);
}

int dc_memory_snapshot_read(const uint8_t *data, size_t size)
{
    DcMemoryWire h; PstR pages;
    uint8_t *ext_new = NULL, *mod_new = NULL;
    if (!dc_memory_decode(data, size, &h, &pages)) return 0;
    uint8_t *owned = malloc(size);
    if (!owned) return 0;
    memcpy(owned, data, size);
    if (!dc_memory_decode(owned, size, &h, &pages)) { free(owned); return 0; }
    /* Allocate both banks before changing existing storage or allocator state. */
    if (!s_ext_ram && h.ext_used) {
        ext_new = calloc(1, (size_t)h.ext_capacity + 16);
        if (!ext_new) { free(owned); return 0; }
    }
    if (!s_mod_ram && h.mod_used) {
        mod_new = calloc(1, (size_t)h.mod_capacity + 16);
        if (!mod_new) { free(ext_new); free(owned); return 0; }
    }
    if (ext_new) s_ext_ram = ext_new;
    if (mod_new) s_mod_ram = mod_new;
    if (s_ext_ram) memset(s_ext_ram, 0, (size_t)h.ext_capacity + 16);
    if (s_mod_ram) memset(s_mod_ram, 0, (size_t)h.mod_capacity + 16);
    /* The immutable input was fully checked before any bank was cleared. */
    if (!dc_memory_pages_read(&pages, s_ext_ram, h.ext_used, h.ext_pages) ||
        !dc_memory_pages_read(&pages, s_mod_ram, h.mod_used, h.mod_pages))
        policy_fail("memory: validated snapshot changed during restore");
    s_ext_hwm = h.ext_used; s_mod_hwm = h.mod_used; s_node_next = h.node_next;
    free(owned);
    return 1;
}
