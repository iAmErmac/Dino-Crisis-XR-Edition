#include "dc_gpu_state.h"
#include "pst_wire.h"
#include <string.h>

static int dc_gpu_state_wire(DcGpuState *s, PstW *w, PstR *r)
{
    uint32_t version = 1;
    if (r ? !pst_r_u32(r, &version) : !pst_w_u32(w, version)) return 0;
    if (version != 1) return 0;
#define FIELD(type, name) if (r ? !pst_r_pod(r, &s->name, sizeof s->name, sizeof(type)) :     !pst_w_pod(w, &s->name, sizeof s->name, sizeof(type))) return 0;
    DC_GPU_STATE_FIELDS(FIELD)
    FIELD(uint32_t, gp0_cmd_buf)
    FIELD(uint32_t, gp0_cmd_addr)
#undef FIELD
    return 1;
}

size_t dc_gpu_state_bytes(void)
{
    DcGpuState s = {0}; PstW w; pst_w_init(&w, NULL, 0);
    return dc_gpu_state_wire(&s, &w, NULL) ? w.written : 0;
}

int dc_gpu_state_validate(const DcGpuState *s)
{
    if (!s || s->gp0_state > 4 || s->gp0_words_collected < 0 ||
        s->gp0_words_collected > 16 || s->gp0_words_needed < 0 || s->gp0_words_needed > 16 ||
        s->polyline_has_prev < 0 || s->polyline_has_prev > 2 ||
        (s->polyline_semi_trans != 0 && s->polyline_semi_trans != 1) ||
        (s->vram_read_active != 0 && s->vram_read_active != 1)) return 0;
#define FLAG(name) if (s->name > 1) return 0;
    FLAG(dither_enabled) FLAG(draw_to_display) FLAG(set_mask_bit) FLAG(check_mask_bit)
    FLAG(interlace_field) FLAG(reverse_flag) FLAG(texture_disable) FLAG(hres2) FLAG(vres)
    FLAG(video_mode) FLAG(display_depth) FLAG(vertical_interlace) FLAG(display_disabled)
    FLAG(irq1_flag) FLAG(lcf)
#undef FLAG
    if (s->texpage_x > 15 || s->texpage_y > 1 || s->semi_transparency > 3 ||
        s->texpage_colors > 3 || s->hres1 > 3 || s->dma_direction > 3 ||
        s->texture_window_value > 0xFFFFF || s->draw_area_left > 1023 ||
        s->draw_area_right > 1023 || s->draw_area_top > 511 || s->draw_area_bottom > 511 ||
        s->draw_offset_x < -1024 || s->draw_offset_x > 1023 ||
        s->draw_offset_y < -1024 || s->draw_offset_y > 1023 ||
        s->display_area_x > 1023 || s->display_area_y > 511 ||
        s->h_display_x1 > 4095 || s->h_display_x2 > 4095 ||
        s->v_display_y1 > 1023 || s->v_display_y2 > 1023) return 0;
    if (s->vram_write_x > 1023 || s->vram_write_y > 511 ||
        s->vram_read_x > 1023 || s->vram_read_y > 511 ||
        s->vram_write_w > 1024 || s->vram_write_h > 512 ||
        s->vram_read_w > 1024 || s->vram_read_h > 512 ||
        s->vram_write_col > s->vram_write_w || s->vram_write_row > s->vram_write_h ||
        s->vram_read_col > s->vram_read_w || s->vram_read_row > s->vram_read_h ||
        s->vram_write_remaining > 262144) return 0;
    if (s->gp0_state == 1 && (!s->gp0_words_collected ||
        s->gp0_words_collected >= s->gp0_words_needed)) return 0;
    if (s->gp0_state == 2 && (!s->vram_write_w || !s->vram_write_h ||
        !s->vram_write_remaining || s->vram_write_col >= s->vram_write_w ||
        s->vram_write_row >= s->vram_write_h)) return 0;
    if (s->vram_read_active && (!s->vram_read_w || !s->vram_read_h ||
        s->vram_read_col >= s->vram_read_w || s->vram_read_row >= s->vram_read_h)) return 0;
    return 1;
}

int dc_gpu_state_write(const DcGpuState *state, uint8_t *data, size_t size)
{
    DcGpuState copy; PstW w;
    if (!data || size != dc_gpu_state_bytes() || !dc_gpu_state_validate(state)) return 0;
    copy = *state; pst_w_init(&w, data, size);
    return dc_gpu_state_wire(&copy, &w, NULL) && w.written == size;
}

int dc_gpu_state_read(const uint8_t *data, size_t size, DcGpuState *state)
{
    DcGpuState copy = {0}; PstR r;
    if (!data || !state || size != dc_gpu_state_bytes()) return 0;
    pst_r_init(&r, data, size);
    if (!dc_gpu_state_wire(&copy, NULL, &r) || r.p != r.end || !dc_gpu_state_validate(&copy)) return 0;
    *state = copy;
    return 1;
}
