#include "dc_call_state.h"
#include "pgxp_hooks.h"

extern int dc_blood_mult(void);
extern int dc_blood_pool_room(void);
extern uint32_t dc_rng_seed_get(void);
extern void dc_rng_seed_set(uint32_t seed);

int dc_blood_call_begin(CPUState *cpu, uint32_t continuation)
{
    DcCallFrame *frame = dc_call_begin(cpu, continuation, DC_CALL_BLOOD);
    frame->values[0] = cpu->gpr[4];
    frame->values[1] = cpu->gpr[31];
    frame->values[2] = dc_rng_seed_get();
    int count = dc_blood_mult();
    frame->values[3] = count > 0 ? (uint32_t)count : 0;
    return count > 0;
}

int dc_blood_call_return(CPUState *cpu, uint32_t continuation)
{
    DcCallFrame *frame = dc_call_resume(cpu, continuation, DC_CALL_BLOOD);
    if (frame->state == DC_CALL_COMPLETE) return 0;
    if (frame->values[3]) ++frame->values[4];
    if (frame->values[4] < frame->values[3] && dc_blood_pool_room()) {
        cpu->gpr[4] = frame->values[0];
        cpu->gpr[31] = frame->values[1];
        psx_pgxp_forget_register(4);
        psx_pgxp_forget_register(31);
        return 1;
    }
    dc_rng_seed_set(frame->values[2]);
    dc_call_finish(frame);
    return 0;
}
