#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "dc_gpu_state.h"
#include "test_process.h"

static void round_trip(const DcGpuState *state, uint8_t *wire, size_t size)
{
    DcGpuState restored; memset(&restored, 0xA5, sizeof restored);
    assert(dc_gpu_state_write(state, wire, size));
    assert(dc_gpu_state_read(wire, size, &restored));
#define CHECK(type, name) assert(restored.name == state->name);
    DC_GPU_STATE_FIELDS(CHECK)
#undef CHECK
    assert(!memcmp(restored.gp0_cmd_buf, state->gp0_cmd_buf, sizeof restored.gp0_cmd_buf));
    assert(!memcmp(restored.gp0_cmd_addr, state->gp0_cmd_addr, sizeof restored.gp0_cmd_addr));
    uint8_t *again = malloc(size); assert(again);
    assert(dc_gpu_state_write(&restored, again, size)); assert(!memcmp(wire, again, size));
    free(again);
}

int main(void)
{
    test_process_init();
    DcGpuState s = {0}, output, before;
    size_t size = dc_gpu_state_bytes();
    uint8_t *wire = malloc(size), *bad = malloc(size); assert(wire && bad && size > 300);
    s.gp0_state = 1; s.gp0_words_needed = 12; s.gp0_words_collected = 7;
    s.gp0_next_source_addr = 0xFFFFFFFFu; s.gp0_cmd_source_addr = 0x80401234u;
    for (unsigned i = 0; i < 16; ++i) {
        s.gp0_cmd_buf[i] = 0x11223344u + i; s.gp0_cmd_addr[i] = 0x80801230u + i * 4;
    }
    s.polyline_color = 0xBEEF; s.polyline_prev_c = 0x7FFF;
    s.polyline_prev_x = -123; s.polyline_prev_y = 456;
    s.polyline_semi_trans = 1; s.polyline_has_prev = 2;
    s.texpage_x = 15; s.texpage_y = 1; s.semi_transparency = 3; s.texpage_colors = 3;
    s.draw_offset_x = -1024; s.draw_offset_y = 1023;
    s.draw_area_right = 1023; s.draw_area_bottom = 511;
    s.h_display_x1 = 4095; s.v_display_y2 = 1023;
    s.display_area_x = 1023; s.display_area_y = 511;
    s.texture_window_value = 0xFFFFF; s.hres1 = 3; s.hres2 = 1;
    s.gpuread_latch = 0xCAFEBABEu; s.g_gp1_flip_seq = 0x1122334455667788ull;
    round_trip(&s, wire, size);
    assert(wire[0] == 1 && wire[1] == 0 && wire[4] == 1 && wire[8] == 7);
    s.gp0_state = 2; s.vram_write_x = 1023; s.vram_write_y = 511;
    s.vram_write_w = 1024; s.vram_write_h = 512; s.vram_write_col = 1000;
    s.vram_write_row = 511; s.vram_write_remaining = 12;
    s.vram_read_active = 1; s.vram_read_x = 1023; s.vram_read_y = 511;
    s.vram_read_w = 1024; s.vram_read_h = 512; s.vram_read_col = 1023; s.vram_read_row = 511;
    round_trip(&s, wire, size);
    s.gp0_state = 4; round_trip(&s, wire, size);
    s.gp0_state = 0; s.vram_read_active = 0; s.vram_read_row = 512;
    s.vram_write_row = 512; s.vram_write_col = 0; s.vram_write_remaining = 0;
    round_trip(&s, wire, size);
    memset(&output, 0xA5, sizeof output); before = output;
    for (size_t n = 0; n < size; ++n) {
        assert(!dc_gpu_state_read(wire, n, &output));
        assert(!memcmp(&before, &output, sizeof output));
    }
    assert(!dc_gpu_state_read(wire, size + 1, &output));
    assert(!dc_gpu_state_read(NULL, size, &output));
    assert(!dc_gpu_state_read(wire, size, NULL));
    memcpy(bad, wire, size); bad[0] = 2;
    assert(!dc_gpu_state_read(bad, size, &output));
    memcpy(bad, wire, size); bad[4] = 5;
    assert(!dc_gpu_state_read(bad, size, &output));
    memcpy(bad, wire, size); memset(bad + 8, 0xFF, 4);
    assert(!dc_gpu_state_read(bad, size, &output));
    assert(!memcmp(&before, &output, sizeof output));
#define REJECT(member, value) do { DcGpuState invalid = s; invalid.member = value;     memset(bad, 0xA5, size); assert(!dc_gpu_state_write(&invalid, bad, size));     for (size_t i = 0; i < size; ++i) assert(bad[i] == 0xA5); } while (0)
    REJECT(gp0_state, 5); REJECT(gp0_words_collected, -1); REJECT(gp0_words_needed, 17);
    REJECT(polyline_has_prev, 3); REJECT(texpage_x, 16); REJECT(texpage_y, 2);
    REJECT(draw_offset_x, -1025); REJECT(video_mode, 2); REJECT(draw_area_bottom, 512);
    REJECT(vram_read_active, 1); REJECT(gp0_state, 2);
#undef REJECT
    free(bad); free(wire); return 0;
}
