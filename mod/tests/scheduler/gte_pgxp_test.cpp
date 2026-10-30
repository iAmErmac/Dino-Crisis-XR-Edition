#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include "gte.h"
#include "pgxp.h"
#include "pgxp_cells.h"
#include "dc_pgxp.h"
#include "dc_projection_cache.h"
#include "dc_gte_capture.h"
#include "dc_gte_camera.h"
#include <cstring>
#include "dc_generated.h"
#include "test_process.h"

extern "C" {
int g_dc_pass_id;
static unsigned latency_calls;
uint32_t psx_gte_cmd_latency(uint32_t command) { return 7 + (command & 63); }
void psx_gte_set(CPUState *cpu, uint32_t latency)
{
    assert(g_gte_site_pc == 0);
    ++latency_calls;
    cpu->gte_ts_done = latency;
}
int gpu_render_projective_enabled(void) { return 0; }
int gpu_ws_present_native_43(void) { return 0; }
int gpu_ws_precise_nclip_enabled(void) { return 0; }
int psx_overlay_shadow_diff_active(void) { return 0; }
int psx_netplay_active(void) { return 0; }
void gpu_pgxp_rederive_enable(void) {}
void psx_ws_note_gte_project(int) {}
uint64_t s_frame_count;
int g_pvx_window;
static int camera_on, camera_wide, camera_behind, camera_far;
static unsigned camera_ticks;
void dc_nots_rtps_tick(void) { ++camera_ticks; }
int dc_nots_get_delta(float *rotation, float *translation)
{
    if (!camera_on) return 0;
    std::memset(rotation, 0, 9 * sizeof *rotation);
    rotation[0] = rotation[4] = rotation[8] = 1;
    translation[0] = 300; translation[1] = 0; translation[2] = camera_far ? 70000.0f : (camera_behind ? -2000.0f : 1000.0f);
    return 1;
}
int dc_ws_world(void) { return camera_wide; }
void dc_ws_frac(int *num, int *den) { assert(camera_wide); *num = 3; *den = 4; }
void psx_fatal_halt(const char*) { std::abort(); }
static unsigned pool_projections;
void pt_poolgeom_rtps(uint32_t, float, float, float depth)
{
    assert(std::isfinite(depth)); ++pool_projections;
    if (g_pvx_window > 0) --g_pvx_window;
}
uint32_t g_debug_last_store_pc;
int psx_mod_gpu_dma_memory_contains(uint32_t, uint32_t) { return 0; }
int pt_poolgeom_is_pool(uint32_t) { return 0; }
void pt_poolgeom_vertex(uint32_t, float, float) { std::abort(); }
int pgxp_lookup(int, int, float*, float*, float*) { return 0; }
}

static void check_vertex(unsigned slot, uint32_t packed, uint32_t origin, float expected_x)
{
    uint32_t address = 0x2000 + slot * 4;
    dc_pgxp_cop2((0x3Au << 26) | ((12 + slot) << 16), packed, address);
    float x, y, depth;
    assert(pgxp_addr_get_vertex(address, packed, &x, &y, &depth) == 2);
    assert(std::fabs(x - expected_x) < 0.01f && std::fabs(y) < 0.01f);
    assert(depth == 1000 && pgxp_addr_vertex_src(address, packed) == origin);
}

static void camera_projection_cases(const char *mode)
{
    using namespace PSXRecomp::GTE;
    GTEState gte{};
    gte.RT[0][0] = gte.RT[1][1] = gte.RT[2][2] = 4096;
    gte.H = 100; gte.DQA = 16;
    int16_t *vertices[3] = {gte.V0, gte.V1, gte.V2};
    for (int i = 0; i < 3; ++i) { vertices[i][0] = (int16_t)(100 + i * 100); vertices[i][2] = 1000; }
    GTEState native = gte;
    dc_gte_camera_reset();
    gte_rtps(&native, 0x0180001u);
    camera_on = 1; camera_wide = !std::strcmp(mode, "camera-wide");
    const bool rewrite = std::strcmp(mode, "camera-no-mac") != 0;
    gte_rtps(&gte, 0x0180001u);
    assert(gte.MAC1 == 100 && gte.IR1 == 100 && gte.SZ[3] == 2000);
    assert((int16_t)gte.SXY[2] == (camera_wide ? 15 : 20));
    assert(gte.MAC0 == native.MAC0 && gte.IR0 == native.IR0);
    float x, y, z;
    assert(dc_projection_cache_get((int16_t)gte.SXY[2], (int16_t)(gte.SXY[2] >> 16), &x, &y, &z));
    assert(std::fabs(x - (camera_wide ? 15.0f : 20.0f)) < 0.01f && z == 2000);
    gte_rtpt(&gte, 0x0180030u);
    assert(gte.SZ[1] == 2000 && gte.SZ[2] == 2000 && gte.SZ[3] == 2000);
    assert(gte.MAC1 == (rewrite ? 600 : 300) && gte.IR1 == (rewrite ? 600 : 300));
    gte.V0[0] = 400;
    gte_rtps(&gte, 0x0180001u);
    assert(gte.MAC1 == (rewrite ? 700 : 400));
    gte.TR[0] = 1;
    gte_rtps(&gte, 0x0180001u);
    assert(gte.MAC1 == 401);
    gte.TR[0] = 0;
    gte_rtps(&gte, 0x0180001u);
    assert(gte.MAC1 == 400);
    camera_far = 1;
    gte_rtps(&gte, 0x0180001u);
    assert(gte.SZ[3] == 65535 && (gte.FLAG & FLAG_SZ3_OTZ));
    camera_far = 0;
    camera_behind = 1; gte.TR[0] = 9;
    const float previous_camera_x = g_cam_T[0];
    gte_rtpt(&gte, 0x0180030u);
    assert(!gte.SZ[1] && !gte.SZ[2] && !gte.SZ[3] && g_dc_prim_crossed);
    assert(g_cam_T[0] == previous_camera_x);
    assert(gte.FLAG & FLAG_DIV_OVF);
    for (unsigned i = 0; i < 3; ++i) {
        uint32_t address = 0x5000 + i * 4, packed = (uint32_t)gte.SXY[i];
        dc_pgxp_cop2((0x3Au << 26) | ((12u + i) << 16), packed, address);
        assert(pgxp_addr_get_vertex(address, packed, &x, &y, &z) == 2);
        assert(z == -1000.0f && std::isfinite(x) && std::isfinite(y));
    }
    camera_behind = 0;
    gte.H = 1000;
    for (int i = 0; i < 3; ++i) vertices[i][2] = 1;
    gte_rtpt(&gte, 0x0180030u);
    assert(!(gte.FLAG & (FLAG_SX2_SAT | FLAG_SY2_SAT | FLAG_SZ3_OTZ | FLAG_DIV_OVF)));
    gte.RT[0][0] = 4096; gte.H = 100;
    dc_gte_camera_reset(); camera_on = 0; camera_wide = 0;
    native = gte;
    gte_rtps(&native, 0x0180001u);
    assert(!g_dc_prim_crossed);
    assert(camera_ticks == 16);

    /* Normal camera-off projection keeps guest registers on the generic path,
     * while the private projection shadow must retain the unshifted view depth. */
    assert(!camera_on && pgxp_preserve_projection() == 0);
    GTEState fractional{};
    fractional.RT[0][0] = 4096; fractional.RT[0][2] = 8192;
    fractional.RT[1][0] = 4096; fractional.RT[1][2] = 2048;
    fractional.RT[2][0] = 2048; fractional.RT[2][2] = 4096;
    fractional.V0[0] = 1; fractional.V0[2] = 1000;
    fractional.H = 100;

    const uint32_t vxy_origin = 0xA000;
    dc_pgxp_cop2((0x32u << 26), 1u, vxy_origin);
    GTEState generic = fractional;
    gte_rtps(&generic, 0x0180001u);
    const uint32_t generic_sxy = (uint32_t)generic.SXY[2];
    const int32_t generic_mac1 = generic.MAC1, generic_mac2 = generic.MAC2,
                  generic_mac3 = generic.MAC3;
    const int16_t generic_ir1 = generic.IR1, generic_ir2 = generic.IR2,
                  generic_ir3 = generic.IR3;
    const uint16_t generic_sz3 = generic.SZ[3];
    const uint32_t generic_flag = generic.FLAG;

    dc_projection_cache_clear();
    pgxp_addr_reset();
    dc_pgxp_cop2((0x32u << 26), 1u, vxy_origin);
    GTEState actual = fractional;
    gte_rtps(&actual, 0x0180001u);
    assert(actual.MAC1 == generic_mac1 && actual.MAC2 == generic_mac2 &&
        actual.MAC3 == generic_mac3 && actual.IR1 == generic_ir1 &&
        actual.IR2 == generic_ir2 && actual.IR3 == generic_ir3 &&
        actual.SZ[3] == generic_sz3 && actual.SXY[2] == (int32_t)generic_sxy &&
        actual.FLAG == generic_flag);
    assert(generic_mac1 == 2001 && generic_mac2 == 501 && generic_mac3 == 1000);
    assert(generic_ir1 == 2001 && generic_ir2 == 501 && generic_ir3 == 1000);
    assert(generic_sz3 == 1000 && generic_sxy == 0x003200C8u && generic_flag == 0);

    const int16_t packed_x = (int16_t)(generic_sxy & 0xFFFFu);
    const int16_t packed_y = (int16_t)(generic_sxy >> 16);
    float cached_x, cached_y, cached_z;
    assert(dc_projection_cache_get(packed_x, packed_y, &cached_x, &cached_y, &cached_z));
    const uint32_t packet_addr = 0x2008;
    dc_pgxp_cop2((0x3Au << 26) | (14u << 16), generic_sxy, packet_addr);
    float fifo_x, fifo_y, fifo_z;
    assert(pgxp_addr_get_vertex(packet_addr, generic_sxy, &fifo_x, &fifo_y, &fifo_z) == 2);
    assert(std::fabs(fifo_x - cached_x) < 0.0001f &&
        std::fabs(fifo_y - cached_y) < 0.0001f && fifo_z == cached_z &&
        pgxp_addr_vertex_src(packet_addr, generic_sxy) == vxy_origin);

    const float expected_x = 100.0f * 2001.0f / 1000.5f;
    const float expected_y = 100.0f * 501.0f / 1000.5f;
    if (std::fabs(cached_x - expected_x) >= 0.0001f ||
        std::fabs(cached_y - expected_y) >= 0.0001f ||
        std::fabs(cached_z - 1000.5f) >= 0.0001f) {
        std::fprintf(stderr, "camera-off projection shadow: got=(%.9g, %.9g, %.9g) expected=(%.9g, %.9g, %.9g)\n",
            cached_x, cached_y, cached_z, expected_x, expected_y, 1000.5f);
        assert(false);
    }
}

int main(int argc, char **argv)
{
    test_process_init();
    dc_gte_capture_reset();
    pgxp_addr_boot();
    assert(g_pgxp_addr_on);
    if (argc > 1) {
        camera_projection_cases(argv[1]);
        std::free(g_px_cell); std::free(g_px_live); return 0;
    }
    using namespace PSXRecomp::GTE;
    GTEState gte{};
    gte.RT[0][0] = gte.RT[1][1] = gte.RT[2][2] = 4096;
    gte.H = 100;
    int16_t* vertices[3] = {gte.V0, gte.V1, gte.V2};
    for (int i = 0; i < 3; ++i) {
        vertices[i][0] = (int16_t)(100 + i * 100);
        vertices[i][2] = 1000;
        dc_pgxp_cop2((0x32u << 26) | ((i * 2u) << 16), 100 + i * 100, 0x9000 + i * 8);
    }
    g_pvx_window = 3;
    gte_rtpt(&gte, 0x0180030u);
    assert(pool_projections == 3 && g_pvx_window == 0);
    assert(pt_current_matrix_epoch() == 1 && g_cam_have);
    assert(g_cam_R[0] == 1 && g_cam_R[4] == 1 && g_cam_R[8] == 1);
    for (unsigned i = 0; i < 3; ++i)
        check_vertex(i, (uint32_t)gte.SXY[i], 0x9000 + i * 8, 10.0f + i * 10.0f);
    assert(g_pgxp_rtp_vi == 0);
    assert(g_pt_proj_h == 100.0f && g_pt_proj_ofx == 0.0f && g_pt_proj_ofy == 0.0f);
    float cached_x, cached_y, cached_z;
    assert(dc_projection_cache_get((int16_t)gte.SXY[2], (int16_t)(gte.SXY[2] >> 16),
        &cached_x, &cached_y, &cached_z));
    assert(std::fabs(cached_x - 30.0f) < 0.1f && cached_z == 1000.0f);
    gte.V0[0] = 400;
    gte_rtps(&gte, 0x0180001u);
    check_vertex(0, (uint32_t)gte.SXY[0], 0x9008, 20.0f);
    check_vertex(1, (uint32_t)gte.SXY[1], 0x9010, 30.0f);
    check_vertex(2, (uint32_t)gte.SXY[2], 0x9000, 40.0f);
    gte.V0[2] = 0;
    gte_rtps(&gte, 0x0180001u);
    dc_pgxp_cop2((0x3Au << 26) | (14u << 16), (uint32_t)gte.SXY[2], 0x3000);
    float x, y, depth;
    assert(pgxp_addr_get_vertex(0x3000, (uint32_t)gte.SXY[2], &x, &y, &depth) == 0);
    check_vertex(0, (uint32_t)gte.SXY[0], 0x9010, 30.0f);
    check_vertex(1, (uint32_t)gte.SXY[1], 0x9000, 40.0f);
    gte.V0[2] = 1000;
    CPUState cpu{};
    for (int i = 0; i < 32; ++i) {
        cpu.gte_data[i] = gte_mfc2(&gte, (uint8_t)i);
        cpu.gte_ctrl[i] = gte_cfc2(&gte, (uint8_t)i);
    }
    float matrix[9], translation[3];
    pt_gte_read_matrix(&cpu, matrix, translation);
    assert(matrix[0] == 1 && matrix[4] == 1 && matrix[8] == 1);
    assert(translation[0] == 0 && translation[1] == 0 && translation[2] == 0);
    gte_execute_pc(&cpu, 0x0180001u, 0x80010000u);
    assert(latency_calls == 1 && cpu.gte_ts_done == 8);
    assert(g_gte_site_pc == 0);
    unsigned before_invalid = pool_projections;
    gte.V0[2] = 0;
    gte_rtps(&gte, 0x0180001u);
    assert(pool_projections == before_invalid);
    gte.V0[2] = 1000;
    gte.TR[0] = -100;
    gte_rtps(&gte, 0x0180001u);
    assert(pt_current_matrix_epoch() == 2 && g_cam_T[0] == -100);
    gte_rtps(&gte, 0x0180001u);
    assert(pt_current_matrix_epoch() == 2);
    cpu.gte_ctrl[0] = 0xF0001000u;
    cpu.gte_ctrl[1] = 0x0800F800u;
    cpu.gte_ctrl[5] = (uint32_t)-123;
    pt_gte_read_matrix(&cpu, matrix, translation);
    assert(matrix[0] == 1 && matrix[1] == -1 && matrix[2] == -0.5f && matrix[3] == 0.5f);
    assert(translation[0] == -123);
    dc_gte_capture_reset();
    assert(!g_cam_have && pt_current_matrix_epoch() == 0 && g_cam_T[0] == 0);
    dc_gte_light_reset();
    cpu.gte_ctrl[8] = 0xF0001000u;
    cpu.gte_ctrl[9] = 0x00000800u;
    cpu.gte_ctrl[16] = 0x00001000u;
    cpu.gte_ctrl[17] = 0x08000000u;
    cpu.gte_ctrl[19] = 0x00000400u;
    gte_execute_pc(&cpu, 0x018001Bu, 0x80010000u);
    float light[9], colors[9], rotation[9], position[3]; long hits = 0;
    float direction[3], color[3];
    const char *light_option = std::getenv("DC_LIGHT_GTE");
    if (light_option && light_option[0] == '1') {
        assert(dc_gte_light_snap(0, light, colors, rotation, position, &hits));
        assert(hits == 1 && light[0] == 1 && light[1] == -1 && light[2] == 0.5f);
        assert(colors[0] == 1 && colors[3] == 0.5f && colors[6] == 0.25f);
        assert(position[0] == -123 && rotation[1] == -1);
        gte_execute_pc(&cpu, 0x018003Fu, 0x80010000u);
        assert(dc_gte_light_snap(0, light, colors, rotation, position, &hits) && hits == 2);
        assert(dc_gte_light_get(direction, color) && direction[1] == -1 && color[2] == 0.25f);
    } else assert(!dc_gte_light_get(direction, color));
    dc_gte_capture_reset();
    assert(!dc_gte_light_get(direction, color));
    std::free(g_px_cell); std::free(g_px_live);
    return 0;
}
