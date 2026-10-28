#ifndef DC_SHOT_H
#define DC_SHOT_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct DcShotState {
    uint32_t enabled, visibility, actor;
    int32_t count, original[3], game_hit, candidate, hit, turning;
    int32_t victims[3], victim_count, blood_event;
    float depth, turn_error;
} DcShotState;
int dc_shot_prepare(uint32_t player, uint32_t parameters, DcShotState *state);
void dc_shot_commit(DcShotState *state, uint32_t blocked);
int dc_shot_blood_actor(int slot, uint32_t *actor);
int dc_actor_model_bones(const uint8_t *ram, uint32_t actor);
int dc_actor_trex_model(const uint8_t *ram, uint32_t actor);
#ifdef __cplusplus
}
#endif
#endif
