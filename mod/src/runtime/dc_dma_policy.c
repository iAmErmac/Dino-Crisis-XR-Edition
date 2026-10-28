#include "dc_dma_policy.h"
#include "pst_wire.h"
#include "crash_trace.h"

extern void dc_meshrep_dma_begin(void);
extern void dc_meshrep_dma_at(uint32_t address);
extern void dc_meshrep_dma(uint32_t mask);
extern void dc_meshrep_otc(uint32_t address);
extern void dcvk_note_submission(void);
extern uint32_t g_ot_lo, g_ot_hi;
extern int g_ot_watch, g_dc_pass_id, g_dc_dma_pass;
static uint32_t s_active, s_mask;

void dc_dma_policy_begin(void)
{
    if (s_active) { psx_fatal_halt("dma: overlapping product submissions"); return; }
    s_active = 1; s_mask = 0;
    dc_meshrep_dma_begin(); dcvk_note_submission();
    g_dc_dma_pass = g_dc_pass_id;
    g_ot_lo = UINT32_MAX; g_ot_hi = 0; g_ot_watch = 1;
}

void dc_dma_policy_node(uint32_t address)
{
    if (!s_active) { psx_fatal_halt("dma: node outside a product submission"); return; }
    uint32_t physical = address & 0x1FFFFFFFu;
    if (physical < g_ot_lo) g_ot_lo = physical;
    if (physical > g_ot_hi) g_ot_hi = physical;
    /* Extra packet banks keep their identity; only console RAM mirrors fold. */
    uint32_t node = physical < 0x00400000u ? physical & 0x001FFFFFu : physical;
    if (node >= 0xB5850u && node < 0xB7850u) {
        s_mask |= 1u << ((node - 0xB5850u) >> 12);
        dc_meshrep_dma_at(node);
    }
}

void dc_dma_policy_finish(void)
{
    if (!s_active) { psx_fatal_halt("dma: completion outside a product submission"); return; }
    dc_meshrep_dma(s_mask);
    s_active = 0; s_mask = 0;
}

void dc_dma_policy_cancel(void)
{
    s_active = 0; s_mask = 0; g_ot_watch = 0;
}

void dc_dma_policy_otc(uint32_t address) { dc_meshrep_otc(address); }

static int dc_dma_policy_decode(const uint8_t *data, size_t size, int active, uint32_t *mask)
{
    uint32_t version, saved_active; PstR r;
    if (!data || size != DC_DMA_POLICY_WIRE_BYTES || (active != 0 && active != 1)) return 0;
    pst_r_init(&r, data, size);
    return pst_r_u32(&r, &version) && version == 1 &&
        pst_r_u32(&r, &saved_active) && saved_active == (uint32_t)active &&
        pst_r_u32(&r, mask) && *mask <= 3 && (saved_active || !*mask);
}

int dc_dma_policy_write(uint8_t *data, size_t size, int active)
{
    PstW w;
    if (!data || size != DC_DMA_POLICY_WIRE_BYTES || (active != 0 && active != 1) ||
        s_active != (uint32_t)active || s_mask > 3 || (!s_active && s_mask)) return 0;
    pst_w_init(&w, data, size);
    return pst_w_u32(&w, 1) && pst_w_u32(&w, s_active) && pst_w_u32(&w, s_mask);
}

int dc_dma_policy_validate(const uint8_t *data, size_t size, int active)
{
    uint32_t mask;
    return dc_dma_policy_decode(data, size, active, &mask);
}

int dc_dma_policy_read(const uint8_t *data, size_t size, int active)
{
    uint32_t mask;
    if (!dc_dma_policy_decode(data, size, active, &mask)) return 0;
    s_active = (uint32_t)active; s_mask = mask;
    return 1;
}
