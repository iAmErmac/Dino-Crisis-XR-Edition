#include "dc_gpu_state.h"
#include "crash_trace.h"
#include "psx_video_timing.h"
extern void sw_post_restore(void);

static void dc_gpu_get_state(DcGpuState *state)
{
    memset(state, 0, sizeof *state);
#define CAPTURE(type, name) _Static_assert(sizeof(name) == sizeof(state->name), "GPU field width mismatch");     memcpy(&state->name, &name, sizeof(name));
    DC_GPU_STATE_FIELDS(CAPTURE)
#undef CAPTURE
    memcpy(state->gp0_cmd_buf, gp0_cmd_buf, sizeof gp0_cmd_buf);
    memcpy(state->gp0_cmd_addr, gp0_cmd_addr, sizeof gp0_cmd_addr);
}

uint32_t gpu_snapshot_bytes(void) { return (uint32_t)dc_gpu_state_bytes(); }
void gpu_snapshot_write(uint8_t *data)
{
    DcGpuState state;
    dc_gpu_get_state(&state);
    if (!dc_gpu_state_write(&state, data, dc_gpu_state_bytes()))
        psx_fatal_halt("gpu: invalid live snapshot state");
}
int gpu_snapshot_validate(const uint8_t *data, uint32_t size)
{
    DcGpuState state;
    return dc_gpu_state_read(data, size, &state);
}

static void dc_gpu_restored(void)
{
    ++s_vram_gen; s_bnf_n = 0;
    if (dc_texprobe_exact()) {
        ++s_tex_epoch;
        s_texprobe_rtt = draw_area_right >= 320 || draw_area_bottom >= 496;
    }
    sw_set_texture_window(texture_window_value);
    sw_set_draw_area((int)draw_area_left, (int)draw_area_top, (int)draw_area_right, (int)draw_area_bottom);
    sw_set_draw_offset(draw_offset_x, draw_offset_y);
    sw_set_mask_bits((int)set_mask_bit, (int)check_mask_bit);
    sw_post_restore(); dc_face_on_load(); f2_on_load();
}

int gpu_snapshot_read(const uint8_t *data, uint32_t size)
{
    DcGpuState state;
    if (!dc_gpu_state_read(data, size, &state)) return 0;
#define RESTORE(type, name) memcpy(&name, &state.name, sizeof(name));
    DC_GPU_STATE_FIELDS(RESTORE)
#undef RESTORE
    memcpy(gp0_cmd_buf, state.gp0_cmd_buf, sizeof gp0_cmd_buf);
    memcpy(gp0_cmd_addr, state.gp0_cmd_addr, sizeof gp0_cmd_addr);
    psx_video_timing_set_pal((int)video_mode);
    dc_gpu_restored();
    return 1;
}

int dc_gpu_capture_vram(uint16_t *pixels, size_t count)
{
    if (!pixels || count != 1024u * 512u) return 0;
    uintptr_t destination = (uintptr_t)pixels, source = (uintptr_t)vram;
    if (destination <= source ? source - destination < sizeof vram :
        destination - source < sizeof vram) return 0;
    if (s_bnf_n) bnf_flush();
    sw_batch_flush();
    memmove(pixels, vram, sizeof vram);
    dc_face_restore_into(pixels); f2_restore_into(pixels);
    return 1;
}

int dc_gpu_restore_vram(const uint16_t *pixels, size_t count)
{
    if (!pixels || count != 1024u * 512u) return 0;
    /* Discard queued work before replacing the authoritative image. */
    s_bnf_n = 0;
    memmove(vram, pixels, sizeof vram);
    dc_gpu_restored();
    return 1;
}
