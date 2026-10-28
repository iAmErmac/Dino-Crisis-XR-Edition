#include "dc_shot.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern uint8_t *memory_get_ram_ptr(void);
extern int dc_nots_active(void), dc_cut_camera(void), dc_aim_turning(float *error);
extern uint32_t dc_aim_geom_first(float *depth);
extern void dc_nots_cam_get(float *yaw, float *pitch, float *distance);
extern void psx_fatal_halt(const char *reason);
extern uint64_t s_frame_count, g_aim_hit_vb;
extern unsigned long long g_shot_log_vb, g_shot_victim_frame, g_shot_turn_vb;
extern int g_presdump_req, g_shot_victim, g_shot_victims[3], g_shot_turn_nv;
extern float g_dc_cam_eye[3], g_aim_hit_pt[3];
extern uint32_t g_aim_hit_actor;
static uint64_t s_shot_point_frame;

enum { PLAYER = 0xB928C, VICTIM_LIST = PLAYER + 588, ACTORS = 0xB7ACC, ACTOR_SIZE = 608 };
static int16_t read_s16(const uint8_t *ram, uint32_t address)
{ return (int16_t)((uint16_t)ram[address] | (uint16_t)ram[address + 1] << 8); }
static uint32_t read_u32(const uint8_t *ram, uint32_t address)
{ return (uint32_t)ram[address] | (uint32_t)ram[address + 1] << 8 | (uint32_t)ram[address + 2] << 16 | (uint32_t)ram[address + 3] << 24; }
static _Noreturn void fail(const char *reason) { psx_fatal_halt(reason); abort(); }

int dc_shot_prepare(uint32_t player, uint32_t parameters, DcShotState *state)
{
    static int enabled = -1;
    static float point_blank;
    if (enabled < 0) {
        const char *value = getenv("DC_SHOT_PICK"); enabled = !(value && *value == '0');
        value = getenv("DC_SHOT_POINTBLANK"); point_blank = value ? (float)atof(value) : 800.f;
    }
    memset(state, 0, sizeof *state);
    state->candidate = state->hit = state->game_hit = -1;
    for (unsigned i = 0; i < 3; ++i) state->original[i] = state->victims[i] = -1;
    if (!enabled || (player & 0x1FFFFFFFu) != PLAYER || !dc_nots_active() || dc_cut_camera()) return 0;
    uint8_t *ram = memory_get_ram_ptr();
    if (!ram || ram[0xB92C8] != 1) return 0;
    state->enabled = 1;
    state->count = ram[VICTIM_LIST] > 3 ? 3 : ram[VICTIM_LIST];
    for (int i = 0; i < state->count; ++i) state->original[i] = ram[VICTIM_LIST + 1 + i];
    state->game_hit = state->count ? state->original[state->count - 1] : -1;
    state->actor = dc_aim_geom_first(&state->depth) & 0x1FFFFFFFu;
    uint32_t actor = state->actor;
    if (actor >= ACTORS && actor < ACTORS + 10u * ACTOR_SIZE && !((actor - ACTORS) % ACTOR_SIZE)) {
        if ((ram[actor + 48] & 1u) && read_s16(ram, actor + 280) > 0 &&
            !(ram[actor + 286] & 0x40u) && !(ram[actor + 373] & 0x80u))
            state->candidate = (int32_t)((actor - ACTORS) / ACTOR_SIZE);
    }
    float px = (float)read_s16(ram, 0xB92AC), pz = (float)read_s16(ram, 0xB92B0);
    state->turning = dc_aim_turning(&state->turn_error);
    if (state->turning) { state->hit = state->game_hit; return 1; }
    int in_list = 0;
    for (int i = 0; i < state->count; ++i) if (state->original[i] == state->candidate) in_list = 1;
    if (state->candidate >= 0 && in_list) state->hit = state->candidate;
    else if (state->candidate >= 0) {
        float zmin = 200.f, zmax = 20200.f;
        uint32_t parameters_address = parameters & 0x1FFFFFFFu;
        if (parameters_address >= 0x10000u && parameters_address + 32u < 0x200000u) {
            zmin = 1e9f; zmax = -1e9f;
            for (unsigned i = 0; i < 4; ++i) {
                float z = (float)read_s16(ram, parameters_address + 8 * i + 4);
                if (z < zmin) zmin = z;
                if (z > zmax) zmax = z;
            }
        }
        float yaw = (float)read_s16(ram, 0xB92B6) * (6.2831853f / 4096.f);
        float forward = ((float)read_s16(ram, actor + 32) - px) * sinf(yaw) +
            ((float)read_s16(ram, actor + 36) - pz) * cosf(yaw);
        if (forward >= zmin && forward <= zmax) state->visibility = 1;
    } else if (state->game_hit >= 0 && state->game_hit < 10 && point_blank > 0.f) {
        uint32_t target = ACTORS + (uint32_t)state->game_hit * ACTOR_SIZE;
        if (hypotf((float)read_s16(ram, target + 32) - px, (float)read_s16(ram, target + 36) - pz) < point_blank)
            state->hit = state->game_hit;
    }
    return 1;
}

void dc_shot_commit(DcShotState *state, uint32_t blocked)
{
    uint8_t *ram = memory_get_ram_ptr();
    if (!ram || !state->enabled) fail("shot: invalid selection commit");
    if (state->visibility) state->hit = (blocked & 0xFFu) ? -1 : state->candidate;
    static int spread = -1;
    if (spread < 0) { const char *value = getenv("DC_SHOT_SPREAD3"); spread = !(value && *value == '0'); }
    int spread_three = spread && (ram[0xB94CC] & 0xF0) == 0x10 && ram[0xB94D1] == 0;
    state->victim_count = 0;
    for (unsigned i = 0; i < 3; ++i) state->victims[i] = -1;
    if (state->hit >= 0) state->victims[state->victim_count++] = state->hit;
    if (spread_three)
        for (int i = 0; i < state->count && state->victim_count < 3; ++i)
            if (state->original[i] >= 0 && state->original[i] < 10 && state->original[i] != state->hit)
                state->victims[state->victim_count++] = state->original[i];
    ram[VICTIM_LIST] = (uint8_t)state->victim_count;
    for (int i = 0; i < state->victim_count; ++i) ram[VICTIM_LIST + 1 + i] = (uint8_t)state->victims[i];
    if (state->victim_count) {
        g_shot_victim = state->victims[0]; g_shot_victim_frame = s_frame_count;
        for (unsigned i = 0; i < 3; ++i) g_shot_victims[i] = state->victims[i];
    }
    if (state->turning) { g_shot_turn_vb = s_frame_count; g_shot_turn_nv = state->victim_count; }
    int shot_call = g_shot_log_vb && s_frame_count - g_shot_log_vb <= 4ull;
    if (state->hit >= 0 && state->hit == state->candidate && state->depth > 0.f &&
        (shot_call || s_frame_count - s_shot_point_frame > 30ull)) {
        float yaw = 0.f, pitch = 0.f, distance = 0.f;
        dc_nots_cam_get(&yaw, &pitch, &distance);
        g_aim_hit_pt[0] = g_dc_cam_eye[0] + sinf(yaw) * cosf(pitch) * state->depth;
        g_aim_hit_pt[1] = g_dc_cam_eye[1] + sinf(pitch) * state->depth;
        g_aim_hit_pt[2] = g_dc_cam_eye[2] + cosf(yaw) * cosf(pitch) * state->depth;
        g_aim_hit_actor = state->actor; g_aim_hit_vb = s_frame_count;
        if (shot_call) s_shot_point_frame = s_frame_count;
    }
    state->blood_event = shot_call;
    if (shot_call) { g_shot_log_vb = 0; g_presdump_req = 1; }
    static int debug = -1;
    if (debug < 0) { const char *value = getenv("DC_SHOTPICK_DBG"); debug = value && *value == '1'; }
    if (debug) fprintf(stderr, "shot: crosshair %d, selected %d, targets %d\n", state->candidate, state->hit, state->victim_count);
}

int dc_actor_model_bones(const uint8_t *ram, uint32_t actor)
{
    actor &= 0x1FFFFFu;
    if (!ram || actor + 160u >= 0x200000u) return -1;
    uint32_t pointer = read_u32(ram, actor + 156);
    if ((pointer >> 24) != 0x80u) return -1;
    uint32_t model = pointer & 0x1FFFFFu;
    if (model + 24u >= 0x200000u || read_s16(ram, model + 22) != 0) return -1;
    return (int)(uint16_t)read_s16(ram, model + 20);
}
int dc_actor_trex_model(const uint8_t *ram, uint32_t actor)
{ int bones = dc_actor_model_bones(ram, actor); return bones == 20 || bones == 10; }

int dc_shot_blood_actor(int slot, uint32_t *actor)
{
    static int enabled = -1;
    if (enabled < 0) { const char *value = getenv("DC_TREX_BLOOD"); enabled = !(value && *value == '0'); }
    uint8_t *ram = memory_get_ram_ptr();
    if (!enabled || !ram || slot < 0 || slot >= 10 || (ram[0xB94CC] & 0xF0) == 0x30) return 0;
    uint32_t address = ACTORS + (uint32_t)slot * ACTOR_SIZE;
    if (!(ram[address + 48] & 1u) || !dc_actor_trex_model(ram, address)) return 0;
    *actor = address;
    return 1;
}
