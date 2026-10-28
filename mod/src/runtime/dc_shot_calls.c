#include "dc_call_state.h"
#include "pgxp_hooks.h"
#include <stdlib.h>
extern void psx_fatal_halt(const char *reason);
extern uint32_t dc_rng_seed_get(void);
extern void dc_rng_seed_set(uint32_t seed);
extern int dc_blood_mult(void), dc_blood_pool_room(void);

static _Noreturn void fail(const char *reason) { psx_fatal_halt(reason); abort(); }
static DcCallFrame *current(CPUState *cpu, uint32_t continuation, uint32_t frame_size)
{
    if ((uint64_t)cpu->gpr[29] + frame_size > 0xFFFFFFFFull) fail("shot: invalid stack identity");
    return dc_call_lookup_stack(cpu, cpu->gpr[29] + frame_size, continuation, DC_CALL_SHOT);
}
void dc_shot_call_begin(CPUState *cpu, uint32_t player, uint32_t parameters,
    uint32_t continuation, uint32_t frame_size, uint32_t visibility_pc, uint32_t blood_pc)
{
    if (!frame_size || (frame_size & 3u) || cpu->gpr[29] < frame_size ||
        !visibility_pc || !blood_pc || ((visibility_pc | blood_pc) & 3u)) fail("shot: invalid call setup");
    DcCallFrame *frame = dc_call_begin(cpu, continuation, DC_CALL_SHOT);
    frame->values[0] = player; frame->values[1] = parameters;
    frame->values[8] = frame_size; frame->values[9] = visibility_pc; frame->values[10] = blood_pc;
}
int dc_shot_call_resuming(CPUState *cpu, uint32_t continuation, uint32_t frame_size)
{
    DcCallFrame *frame = current(cpu, continuation, frame_size);
    return frame && (frame->state == DC_CALL_COMPLETE || frame->values[2] != 0);
}
static uint32_t queue_blood(CPUState *cpu, DcCallFrame *frame)
{
    cpu->gpr[4] = 0x80000000u | frame->values[7]; cpu->gpr[31] = frame->continuation;
    psx_pgxp_forget_register(4); psx_pgxp_forget_register(31);
    return frame->values[10];
}
uint32_t dc_shot_call_step(CPUState *cpu, uint32_t continuation, uint32_t frame_size)
{
    DcCallFrame *frame = current(cpu, continuation, frame_size);
    if (!frame) fail("shot: missing call context");
    if (frame->state == DC_CALL_COMPLETE) return 0;
    if (frame->values[8] != frame_size) fail("shot: changed stack frame size");
    if (frame->values[2] == 0) {
        dc_guest_registers_capture(cpu, &frame->guest);
        if (!dc_shot_prepare(frame->values[0], frame->values[1], &frame->shot)) {
            dc_call_finish(frame); return 0;
        }
        if (frame->shot.visibility) {
            frame->values[2] = 1;
            cpu->gpr[4] = 0x800B92ACu; cpu->gpr[5] = 0x80000000u | (frame->shot.actor + 32u);
            cpu->gpr[6] = 5; cpu->gpr[31] = continuation;
            psx_pgxp_forget_register(4); psx_pgxp_forget_register(5);
            psx_pgxp_forget_register(6); psx_pgxp_forget_register(31);
            return frame->values[9];
        }
        dc_shot_commit(&frame->shot, 0); frame->values[2] = 2;
    } else if (frame->values[2] == 1) {
        uint32_t blocked = cpu->gpr[2] & 0xFFu;
        dc_guest_registers_restore(cpu, &frame->guest);
        dc_shot_commit(&frame->shot, blocked); frame->values[2] = 2;
    } else if (frame->values[2] == 2 && frame->values[4]) {
        ++frame->values[5];
        if (frame->values[5] < frame->values[4] && dc_blood_pool_room()) return queue_blood(cpu, frame);
        dc_rng_seed_set(frame->values[6]);
        dc_guest_registers_restore(cpu, &frame->guest);
        ++frame->values[3]; frame->values[4] = frame->values[5] = 0;
    }
    if (frame->shot.blood_event) {
        while (frame->values[3] < (uint32_t)frame->shot.victim_count) {
            uint32_t actor;
            if (!dc_shot_blood_actor(frame->shot.victims[frame->values[3]], &actor)) { ++frame->values[3]; continue; }
            frame->values[7] = actor; frame->values[6] = dc_rng_seed_get();
            int count = dc_blood_mult(); frame->values[4] = count > 0 ? (uint32_t)count : 1;
            frame->values[5] = 0;
            if (dc_blood_pool_room()) return queue_blood(cpu, frame);
            dc_rng_seed_set(frame->values[6]);
            dc_guest_registers_restore(cpu, &frame->guest);
            ++frame->values[3]; frame->values[4] = 0;
        }
    }
    dc_call_finish(frame);
    return 0;
}
