#ifndef DC_CALL_STATE_H
#define DC_CALL_STATE_H

#include "cpu_state.h"
#include "dc_draw_pose.h"
#include "dc_sound_swap.h"
#include "dc_mesh_runs.h"
#include "dc_guest_registers.h"
#include "dc_shot.h"
#ifdef __cplusplus
extern "C" {
#endif

enum { DC_CALL_STATE_VERSION = 9, DC_CALL_CAPACITY = 64,
       DC_CALL_ANIMATION = 1, DC_CALL_BLOOD = 2, DC_CALL_DRAW_POSE = 3, DC_CALL_SOUND = 4, DC_CALL_MESH = 5, DC_CALL_GRENADE = 6, DC_CALL_SHOT = 7, DC_CALL_CREATURE_BLOOD = 8,
       DC_CALL_BACKGROUND = 9, DC_CALL_ACTIVE = 1, DC_CALL_COMPLETE = 2 };
typedef struct DcCallFrame {
    uint32_t kind, owner, stack, continuation, state;
    uint32_t values[16];
    DcDrawPose pose;
    DcSoundSwap sound;
    DcMeshRuns mesh;
    DcGuestRegisters guest;
    DcShotState shot;
} DcCallFrame;
typedef struct DcCallState {
    uint32_t version;
    DcCallFrame frames[DC_CALL_CAPACITY];
} DcCallState;

int dc_call_has_kind(uint32_t kind);
DcCallFrame *dc_call_active(CPUState *cpu, uint32_t kind);
DcCallFrame *dc_call_begin(CPUState *cpu, uint32_t continuation, uint32_t kind);
DcCallFrame *dc_call_lookup(CPUState *cpu, uint32_t continuation, uint32_t kind);
DcCallFrame *dc_call_lookup_stack(CPUState *cpu, uint32_t stack, uint32_t continuation, uint32_t kind);
DcCallFrame *dc_call_resume(CPUState *cpu, uint32_t continuation, uint32_t kind);
void dc_call_finish(DcCallFrame *frame);
void dc_call_clear(DcCallFrame *frame);
void dc_call_discard_thread(uint32_t tcb);
void dc_call_reset(void);
void dc_call_capture(DcCallState *state);
int dc_call_validate(const DcCallState *state);
int dc_call_restore(const DcCallState *state);
void dc_anim_call_begin(CPUState *cpu, uint32_t continuation);
void dc_anim_call_end(CPUState *cpu, uint32_t continuation);
int dc_blood_call_begin(CPUState *cpu, uint32_t continuation);
int dc_blood_call_return(CPUState *cpu, uint32_t continuation);
void dc_pose_call_begin(CPUState *cpu, uint32_t continuation);
void dc_pose_call_end(CPUState *cpu, uint32_t continuation);
int dc_call_sound_active(void);
void dc_sound_call_begin(CPUState *cpu);
void dc_sound_call_end(CPUState *cpu, uint32_t continuation);
uint32_t dc_mesh_call_begin(CPUState *cpu, uint32_t header, uint32_t triangle_pc,
    uint32_t triangle_return, uint32_t quad_pc, uint32_t quad_return);
int dc_mesh_call_pending(CPUState *cpu, uint32_t continuation);
uint32_t dc_mesh_call_next(CPUState *cpu, uint32_t continuation);
uint32_t dc_grenade_entry(CPUState *cpu, uint32_t continuation, uint32_t callee);
void dc_shot_call_begin(CPUState *cpu, uint32_t player, uint32_t parameters,
    uint32_t continuation, uint32_t frame_size, uint32_t visibility_pc, uint32_t blood_pc);
int dc_shot_call_resuming(CPUState *cpu, uint32_t continuation, uint32_t frame_size);
uint32_t dc_shot_call_step(CPUState *cpu, uint32_t continuation, uint32_t frame_size);

#ifdef __cplusplus
}
#endif
#endif
