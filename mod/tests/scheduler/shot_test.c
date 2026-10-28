#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dc_shot.h"
#include "test_process.h"

enum {
    RAM_SIZE = 0x200000,
    PLAYER = 0xB928C,
    VICTIM_LIST = PLAYER + 588,
    ACTORS = 0xB7ACC,
    ACTOR_SIZE = 608,
    PARAMS = 0x80010000u,
    PARAMS_PHYS = 0x10000
};

static uint8_t ram[RAM_SIZE];
uint64_t s_frame_count;
uint64_t g_aim_hit_vb;
unsigned long long g_shot_log_vb;
unsigned long long g_shot_victim_frame;
unsigned long long g_shot_turn_vb;
int g_presdump_req;
int g_shot_victim;
int g_shot_victims[3];
int g_shot_turn_nv;
float g_dc_cam_eye[3];
float g_aim_hit_pt[3];
uint32_t g_aim_hit_actor;

static int mode_active;
static int cut_camera;
static int turning;
static float turning_error;
static uint32_t geometry_actor;
static float geometry_depth;
static float camera_yaw;
static float camera_pitch;
static float camera_distance;
static unsigned geometry_calls;

uint8_t *memory_get_ram_ptr(void)
{
    return ram;
}

int dc_nots_active(void)
{
    return mode_active;
}

int dc_cut_camera(void)
{
    return cut_camera;
}

int dc_aim_turning(float *error)
{
    if (error) *error = turning_error;
    return turning;
}

uint32_t dc_aim_geom_first(float *depth)
{
    ++geometry_calls;
    if (depth) *depth = geometry_depth;
    return geometry_actor;
}

void dc_nots_cam_get(float *yaw, float *pitch, float *distance)
{
    if (yaw) *yaw = camera_yaw;
    if (pitch) *pitch = camera_pitch;
    if (distance) *distance = camera_distance;
}

void psx_fatal_halt(const char *reason)
{
    fprintf(stderr, "unexpected shot fatal: %s\n", reason ? reason : "(null)");
    abort();
}

static void set_env(const char *name, const char *value)
{
#ifdef _WIN32
    assert(_putenv_s(name, value) == 0);
#else
    assert(setenv(name, value, 1) == 0);
#endif
}

static void write_s16(uint32_t address, int value)
{
    uint16_t word = (uint16_t)(int16_t)value;
    assert(address + 1u < RAM_SIZE);
    ram[address] = (uint8_t)word;
    ram[address + 1] = (uint8_t)(word >> 8);
}

static void write_u32(uint32_t address, uint32_t value)
{
    assert(address + 3u < RAM_SIZE);
    ram[address] = (uint8_t)value;
    ram[address + 1] = (uint8_t)(value >> 8);
    ram[address + 2] = (uint8_t)(value >> 16);
    ram[address + 3] = (uint8_t)(value >> 24);
}

static uint32_t actor_address(int slot)
{
    assert(slot >= 0 && slot < 10);
    return ACTORS + (uint32_t)slot * ACTOR_SIZE;
}

static void set_actor(int slot, int x, int z, int bones, int alive)
{
    const uint32_t actor = actor_address(slot);
    const uint32_t model = 0x180000u + (uint32_t)slot * 64u;
    ram[actor + 48] = alive ? 1u : 0u;
    write_s16(actor + 32, x);
    write_s16(actor + 36, z);
    write_s16(actor + 280, alive ? 1000 : 0);
    ram[actor + 286] = 0;
    ram[actor + 373] = 0;
    write_u32(actor + 156, 0x80000000u | model);
    write_s16(model + 20, bones);
    write_s16(model + 22, 0);
}

static void set_victim_list(unsigned count, const int *victims)
{
    assert(count <= 3);
    ram[VICTIM_LIST] = (uint8_t)count;
    for (unsigned i = 0; i < count; ++i) {
        assert(victims[i] >= 0 && victims[i] < 10);
        ram[VICTIM_LIST + 1u + i] = (uint8_t)victims[i];
    }
}

static void set_parameters(int z0, int z1, int z2, int z3)
{
    const int values[4] = {z0, z1, z2, z3};
    for (unsigned i = 0; i < 4; ++i)
        write_s16(PARAMS_PHYS + 8u * i + 4u, values[i]);
}

static void reset_world(void)
{
    memset(ram, 0, sizeof ram);
    mode_active = 1;
    cut_camera = 0;
    turning = 0;
    turning_error = 0.f;
    geometry_actor = 0;
    geometry_depth = 0.f;
    camera_yaw = 0.f;
    camera_pitch = 0.f;
    camera_distance = 1000.f;
    geometry_calls = 0;
    s_frame_count = 100;
    g_aim_hit_vb = 0;
    g_shot_log_vb = 0;
    g_shot_victim_frame = 0;
    g_shot_turn_vb = 0;
    g_presdump_req = 0;
    g_shot_victim = -1;
    g_shot_victims[0] = g_shot_victims[1] = g_shot_victims[2] = -1;
    g_shot_turn_nv = 0;
    g_dc_cam_eye[0] = 10.f;
    g_dc_cam_eye[1] = 20.f;
    g_dc_cam_eye[2] = 30.f;
    g_aim_hit_pt[0] = g_aim_hit_pt[1] = g_aim_hit_pt[2] = 0.f;
    g_aim_hit_actor = 0;
    ram[0xB92C8] = 1;
    write_s16(0xB92AC, 0);
    write_s16(0xB92B0, 0);
    write_s16(0xB92B6, 0);
    ram[0xB94CC] = 0x20;
    ram[0xB94D1] = 0;
    set_parameters(200, 20200, 200, 20200);
}

static DcShotState prepare(uint32_t player)
{
    DcShotState state;
    assert(dc_shot_prepare(player, PARAMS, &state) == 1);
    return state;
}

static void assert_victims(const DcShotState *state, unsigned count, const int *victims)
{
    assert(state->victim_count == (int)count);
    assert(ram[VICTIM_LIST] == count);
    for (unsigned i = 0; i < count; ++i) {
        assert(state->victims[i] == victims[i]);
        assert(ram[VICTIM_LIST + 1u + i] == (uint8_t)victims[i]);
    }
}

static void test_context_gates(void)
{
    DcShotState state;
    reset_world();
    mode_active = 0;
    assert(dc_shot_prepare(0x8000B928Cu, PARAMS, &state) == 0);
    assert(!state.enabled && geometry_calls == 0);

    reset_world();
    cut_camera = 1;
    assert(dc_shot_prepare(0x8000B928Cu, PARAMS, &state) == 0);
    assert(!state.enabled && geometry_calls == 0);

    reset_world();
    assert(dc_shot_prepare(0x8000B920u, PARAMS, &state) == 0);
    assert(!state.enabled && geometry_calls == 0);

    reset_world();
    ram[0xB92C8] = 0;
    assert(dc_shot_prepare(0x8000B928Cu, PARAMS, &state) == 0);
    assert(!state.enabled && geometry_calls == 0);
}

static void test_crosshair_in_game_list(void)
{
    const int original[] = {1, 2};
    const int expected[] = {1};
    reset_world();
    set_actor(1, 0, 1000, 21, 1);
    set_victim_list(2, original);
    geometry_actor = actor_address(1);
    geometry_depth = 1000.f;
    DcShotState state = prepare(0x8000B928Cu);
    assert(state.enabled && state.candidate == 1 && state.hit == 1);
    assert(!state.visibility && state.count == 2 && state.game_hit == 2);
    dc_shot_commit(&state, 0xFFu);
    assert_victims(&state, 1, expected);
    assert(g_shot_victim == 1 && g_shot_victim_frame == s_frame_count);
}

static void test_turning_keeps_game_victim(void)
{
    const int original[] = {1, 3};
    const int expected[] = {3};
    reset_world();
    set_actor(1, 0, 1000, 21, 1);
    set_victim_list(2, original);
    geometry_actor = actor_address(1);
    turning = 1;
    turning_error = 25.f;
    DcShotState state = prepare(0x8000B928Cu);
    assert(state.turning && state.turn_error == 25.f);
    assert(state.hit == 3 && !state.visibility);
    dc_shot_commit(&state, 0);
    assert_victims(&state, 1, expected);
    assert(g_shot_turn_vb == s_frame_count && g_shot_turn_nv == 1);
}

static void test_visibility_blocked_and_visible(void)
{
    const int original[] = {2};
    const int expected[] = {1};
    reset_world();
    set_actor(1, 0, 1000, 21, 1);
    set_victim_list(1, original);
    geometry_actor = actor_address(1);
    geometry_depth = 1000.f;
    DcShotState state = prepare(0x8000B928Cu);
    assert(state.candidate == 1 && state.visibility && state.hit == -1);
    dc_shot_commit(&state, 1);
    assert_victims(&state, 0, NULL);
    assert(g_shot_victim == -1);

    reset_world();
    set_actor(1, 0, 1000, 21, 1);
    set_victim_list(1, original);
    geometry_actor = actor_address(1);
    geometry_depth = 1000.f;
    state = prepare(0x8000B928Cu);
    assert(state.visibility && state.candidate == 1);
    dc_shot_commit(&state, 0);
    assert_victims(&state, 1, expected);
    assert(g_shot_victim == 1 && g_shot_victims[0] == 1);
}

static void test_out_of_range_does_not_request_visibility(void)
{
    const int original[] = {2};
    reset_world();
    set_actor(1, 0, 1000, 21, 1);
    set_victim_list(1, original);
    set_parameters(2000, 3000, 2000, 3000);
    geometry_actor = actor_address(1);
    geometry_depth = 1000.f;
    DcShotState state = prepare(0x8000B928Cu);
    assert(state.candidate == 1 && !state.visibility && state.hit == -1);
    dc_shot_commit(&state, 0);
    assert_victims(&state, 0, NULL);
}

static void test_point_blank_fallback(void)
{
    const int original[] = {2};
    const int expected[] = {2};
    reset_world();
    set_actor(2, 100, 100, 21, 1);
    set_victim_list(1, original);
    geometry_actor = 0;
    geometry_depth = 0.f;
    DcShotState state = prepare(0x8000B928Cu);
    assert(state.candidate == -1 && state.hit == 2 && !state.visibility);
    dc_shot_commit(&state, 0);
    assert_victims(&state, 1, expected);
}

static void test_buckshot_slug_and_dart_spread(void)
{
    const int original[] = {1, 2, 3};
    const int three[] = {1, 2, 3};
    const int one[] = {1};

    reset_world();
    set_actor(1, 0, 1000, 21, 1);
    set_victim_list(3, original);
    geometry_actor = actor_address(1);
    ram[0xB94CC] = 0x10;
    ram[0xB94D1] = 0;
    DcShotState state = prepare(0x8000B928Cu);
    dc_shot_commit(&state, 0);
    assert_victims(&state, 3, three);

    reset_world();
    set_actor(1, 0, 1000, 21, 1);
    set_victim_list(3, original);
    geometry_actor = actor_address(1);
    ram[0xB94CC] = 0x10;
    ram[0xB94D1] = 1;
    state = prepare(0x8000B928Cu);
    dc_shot_commit(&state, 0);
    assert_victims(&state, 1, one);

    reset_world();
    set_actor(1, 0, 1000, 21, 1);
    set_victim_list(3, original);
    geometry_actor = actor_address(1);
    ram[0xB94CC] = 0x20;
    ram[0xB94D1] = 0;
    state = prepare(0x8000B928Cu);
    dc_shot_commit(&state, 0);
    assert_victims(&state, 1, one);
}

static void test_hit_point_and_shot_event(void)
{
    const int original[] = {1};
    const int expected[] = {1};
    reset_world();
    set_actor(1, 0, 1000, 21, 1);
    set_victim_list(1, original);
    geometry_actor = actor_address(1);
    geometry_depth = 1000.f;
    camera_yaw = 0.f;
    camera_pitch = 0.f;
    g_shot_log_vb = s_frame_count;
    g_presdump_req = 0;
    DcShotState state = prepare(0x8000B928Cu);
    dc_shot_commit(&state, 0);
    assert_victims(&state, 1, expected);
    assert(state.blood_event == 1);
    assert(g_shot_log_vb == 0 && g_presdump_req == 1);
    assert(g_aim_hit_actor == actor_address(1) && g_aim_hit_vb == s_frame_count);
    assert(fabsf(g_aim_hit_pt[0] - 10.f) < 0.01f);
    assert(fabsf(g_aim_hit_pt[1] - 20.f) < 0.01f);
    assert(fabsf(g_aim_hit_pt[2] - 1030.f) < 0.01f);
}

static void test_no_hit_retains_previous_history(void)
{
    reset_world();
    g_shot_victim = 4;
    g_shot_victim_frame = 88;
    g_shot_victims[0] = 4;
    g_shot_victims[1] = 5;
    g_shot_victims[2] = -1;
    geometry_actor = 0;
    set_victim_list(0, NULL);
    DcShotState state = prepare(0x8000B928Cu);
    assert(state.hit == -1 && state.candidate == -1);
    dc_shot_commit(&state, 0);
    assert_victims(&state, 0, NULL);
    assert(g_shot_victim == 4 && g_shot_victim_frame == 88);
    assert(g_shot_victims[0] == 4 && g_shot_victims[1] == 5 && g_shot_victims[2] == -1);
}

static void test_trex_model_eligibility(void)
{
    uint32_t actor = 0;
    reset_world();
    set_actor(2, 0, 100, 20, 1);
    ram[0xB94CC] = 0x10;
    assert(dc_shot_blood_actor(2, &actor) == 1 && actor == actor_address(2));
    assert(dc_actor_trex_model(ram, actor));

    set_actor(2, 0, 100, 10, 1);
    assert(dc_shot_blood_actor(2, &actor) == 1 && actor == actor_address(2));

    set_actor(2, 0, 100, 21, 1);
    assert(!dc_actor_trex_model(ram, actor));
    assert(dc_shot_blood_actor(2, &actor) == 0);

    set_actor(2, 0, 100, 20, 1);
    ram[0xB94CC] = 0x30;
    assert(dc_shot_blood_actor(2, &actor) == 0);

    ram[0xB94CC] = 0x10;
    ram[actor + 48] = 0;
    assert(dc_shot_blood_actor(2, &actor) == 0);
    assert(dc_shot_blood_actor(-1, &actor) == 0);
    assert(dc_shot_blood_actor(10, &actor) == 0);
}

static void test_disabled_feature_process(void)
{
    DcShotState state;
    reset_world();
    assert(dc_shot_prepare(0x8000B928Cu, PARAMS, &state) == 0);
    assert(!state.enabled && geometry_calls == 0);
}

int main(int argc, char **argv)
{
    test_process_init();
    if (argc == 2 && strcmp(argv[1], "disabled") == 0) {
        set_env("DC_SHOT_PICK", "0");
        test_disabled_feature_process();
        return 0;
    }

    set_env("DC_SHOT_PICK", "1");
    set_env("DC_SHOT_POINTBLANK", "800");
    set_env("DC_SHOT_SPREAD3", "1");
    set_env("DC_TREX_BLOOD", "1");
    set_env("DC_SHOTPICK_DBG", "0");
    test_context_gates();
    test_crosshair_in_game_list();
    test_turning_keeps_game_victim();
    test_visibility_blocked_and_visible();
    test_out_of_range_does_not_request_visibility();
    test_point_blank_fallback();
    test_buckshot_slug_and_dart_spread();
    test_hit_point_and_shot_event();
    test_no_hit_retains_previous_history();
    test_trex_model_eligibility();
    return 0;
}
