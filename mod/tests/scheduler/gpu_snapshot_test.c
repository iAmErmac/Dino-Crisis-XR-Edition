#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "dc_gpu_state.h"
#include "test_process.h"

#define DECLARE(type, name) static type name;
DC_GPU_STATE_FIELDS(DECLARE)
#undef DECLARE
static uint32_t gp0_cmd_buf[16], gp0_cmd_addr[16];
static uint16_t vram[1024 * 512];
static unsigned s_vram_gen, s_tex_epoch;
static int s_bnf_n, s_texprobe_rtt, batch_pending;
static unsigned barriers, rebuilds, faces, video_changes;
static int pal;
/* Controlled renderer collaborators expose ordering and pixel ownership. */
static int dc_texprobe_exact(void) { return 1; }
static void bnf_flush(void) { assert(s_bnf_n); ++barriers; vram[0] = 0x1111; s_bnf_n = 0; }
static void sw_batch_flush(void) { ++barriers; if (batch_pending) vram[1] = 0x2222; batch_pending = 0; }
static void sw_set_texture_window(uint32_t value) { assert(value == texture_window_value); }
static void sw_set_draw_area(int l, int t, int r, int b)
{ assert(l == draw_area_left && t == draw_area_top && r == draw_area_right && b == draw_area_bottom); }
static void sw_set_draw_offset(int x, int y) { assert(x == draw_offset_x && y == draw_offset_y); }
static void sw_set_mask_bits(int set, int check) { assert(set == set_mask_bit && check == check_mask_bit); }
void sw_post_restore(void) { ++rebuilds; batch_pending = 0; }
static void dc_face_restore_into(uint16_t *p) { p[2] = 0x3333; }
static void f2_restore_into(uint16_t *p) { p[3] = 0x4444; }
static void dc_face_on_load(void) { ++faces; }
static void f2_on_load(void) { ++faces; }
int psx_video_timing_set_pal(int value) { pal = value; ++video_changes; return 1; }
void psx_fatal_halt(const char *reason) { (void)reason; abort(); }
#include "dc_gpu_snapshot.inc"

int main(void)
{
    test_process_init();
    size_t bytes = gpu_snapshot_bytes(), count = 1024u * 512u;
    uint8_t *wire = malloc(bytes), *bad = malloc(bytes);
    uint16_t *pixels = malloc(count * sizeof *pixels); assert(wire && bad && pixels);
    gp0_state = 1; gp0_words_collected = 2; gp0_words_needed = 3;
    gp0_cmd_source_addr = 0x80401000u; gp0_next_source_addr = 0xFFFFFFFFu;
    for (unsigned i = 0; i < 16; ++i) { gp0_cmd_buf[i] = 0x12345678u + i; gp0_cmd_addr[i] = 0x80401000u + i * 4; }
    set_mask_bit = 1; check_mask_bit = 1; texture_window_value = 0x23456;
    draw_area_right = 1023; draw_area_bottom = 511; draw_offset_x = -300;
    video_mode = 1; g_gp1_flip_seq = 0xABCDEF0123456789ull;
    gpu_snapshot_write(wire); assert(gpu_snapshot_validate(wire, (uint32_t)bytes));
    gp0_cmd_buf[15] = 0; gp0_cmd_addr[15] = 0; gp0_state = 0;
    video_mode = 0; draw_offset_x = 0; s_bnf_n = 2; batch_pending = 1;
    assert(gpu_snapshot_read(wire, (uint32_t)bytes));
    assert(gp0_state == 1 && gp0_cmd_addr[15] == 0x8040103Cu && gp0_cmd_buf[15] == 0x12345687u);
    assert(pal == 1 && video_changes == 1 && rebuilds == 1 && faces == 2);
    assert(!s_bnf_n && !batch_pending && s_vram_gen == 1 && s_tex_epoch == 1 && s_texprobe_rtt);
    assert(g_gp1_flip_seq == 0xABCDEF0123456789ull);
    memcpy(bad, wire, bytes); bad[0] = 2;
    assert(!gpu_snapshot_read(bad, (uint32_t)bytes));
    assert(video_changes == 1 && rebuilds == 1 && faces == 2);
    s_bnf_n = 1; batch_pending = 1;
    vram[2] = 0xAAAA; vram[3] = 0xBBBB;
    assert(!dc_gpu_capture_vram(vram, count));
    assert(s_bnf_n == 1 && batch_pending == 1 && barriers == 0);
    assert(!dc_gpu_capture_vram(pixels, count - 1));
    assert(dc_gpu_capture_vram(pixels, count));
    assert(barriers == 2 && pixels[0] == 0x1111 && pixels[1] == 0x2222);
    assert(pixels[2] == 0x3333 && pixels[3] == 0x4444);
    assert(vram[2] == 0xAAAA && vram[3] == 0xBBBB);
    s_bnf_n = 1; batch_pending = 1;
    assert(!dc_gpu_restore_vram(pixels, count - 1));
    assert(s_bnf_n == 1 && rebuilds == 1);
    assert(dc_gpu_restore_vram(pixels, count));
    assert(vram[2] == 0x3333 && vram[3] == 0x4444 && !s_bnf_n && !batch_pending);
    assert(barriers == 2 && rebuilds == 2 && faces == 4);
    free(pixels); free(bad); free(wire); return 0;
}
