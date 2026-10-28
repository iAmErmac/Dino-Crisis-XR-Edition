#ifndef DC_GPU_STATE_H
#define DC_GPU_STATE_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define DC_GPU_STATE_FIELDS(X) \
    X(uint32_t, gp0_state) \
    X(int32_t, gp0_words_collected) \
    X(int32_t, gp0_words_needed) \
    X(uint32_t, gp0_next_source_addr) \
    X(uint32_t, gp0_cmd_source_addr) \
    X(uint16_t, polyline_color) \
    X(int32_t, polyline_prev_x) \
    X(int32_t, polyline_prev_y) \
    X(uint16_t, polyline_prev_c) \
    X(int32_t, polyline_semi_trans) \
    X(int32_t, polyline_has_prev) \
    X(uint16_t, vram_write_x) \
    X(uint16_t, vram_write_y) \
    X(uint16_t, vram_write_w) \
    X(uint16_t, vram_write_h) \
    X(uint16_t, vram_write_col) \
    X(uint16_t, vram_write_row) \
    X(uint32_t, vram_write_remaining) \
    X(int32_t, vram_read_active) \
    X(uint16_t, vram_read_x) \
    X(uint16_t, vram_read_y) \
    X(uint16_t, vram_read_w) \
    X(uint16_t, vram_read_h) \
    X(uint16_t, vram_read_col) \
    X(uint16_t, vram_read_row) \
    X(uint32_t, texpage_x) \
    X(uint32_t, texpage_y) \
    X(uint32_t, semi_transparency) \
    X(uint32_t, texpage_colors) \
    X(uint32_t, dither_enabled) \
    X(uint32_t, draw_to_display) \
    X(uint32_t, set_mask_bit) \
    X(uint32_t, check_mask_bit) \
    X(uint32_t, interlace_field) \
    X(uint32_t, reverse_flag) \
    X(uint32_t, texture_disable) \
    X(uint32_t, draw_area_left) \
    X(uint32_t, draw_area_top) \
    X(uint32_t, draw_area_right) \
    X(uint32_t, draw_area_bottom) \
    X(int32_t, draw_offset_x) \
    X(int32_t, draw_offset_y) \
    X(uint32_t, texture_window_value) \
    X(uint32_t, hres2) \
    X(uint32_t, hres1) \
    X(uint32_t, vres) \
    X(uint32_t, video_mode) \
    X(uint32_t, display_depth) \
    X(uint32_t, vertical_interlace) \
    X(uint32_t, display_disabled) \
    X(uint32_t, irq1_flag) \
    X(uint32_t, dma_direction) \
    X(uint32_t, lcf) \
    X(uint32_t, display_area_x) \
    X(uint32_t, display_area_y) \
    X(uint32_t, h_display_x1) \
    X(uint32_t, h_display_x2) \
    X(uint32_t, v_display_y1) \
    X(uint32_t, v_display_y2) \
    X(uint32_t, gpuread_latch) \
    X(uint64_t, g_gp1_flip_seq)

typedef struct DcGpuState {
#define DC_GPU_DECLARE(type, name) type name;
    DC_GPU_STATE_FIELDS(DC_GPU_DECLARE)
#undef DC_GPU_DECLARE
    uint32_t gp0_cmd_buf[16], gp0_cmd_addr[16];
} DcGpuState;
size_t dc_gpu_state_bytes(void);
int dc_gpu_state_validate(const DcGpuState *state);
int dc_gpu_state_write(const DcGpuState *state, uint8_t *data, size_t size);
int dc_gpu_state_read(const uint8_t *data, size_t size, DcGpuState *state);
uint32_t gpu_snapshot_bytes(void);
void gpu_snapshot_write(uint8_t *data);
int gpu_snapshot_validate(const uint8_t *data, uint32_t size);
int gpu_snapshot_read(const uint8_t *data, uint32_t size);
int dc_gpu_capture_vram(uint16_t *pixels, size_t count);
int dc_gpu_restore_vram(const uint16_t *pixels, size_t count);
#ifdef __cplusplus
}
#endif
#endif
