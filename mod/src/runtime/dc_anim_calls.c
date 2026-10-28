#include "dc_call_state.h"
extern void pt_anim_frame(CPUState *cpu, uint32_t source, uint32_t destination, uint32_t bones);

void dc_anim_call_begin(CPUState *cpu, uint32_t continuation)
{
    DcCallFrame *frame = dc_call_begin(cpu, continuation, DC_CALL_ANIMATION);
    for (unsigned i = 0; i < 3; ++i) frame->values[i] = cpu->gpr[4 + i];
}

void dc_anim_call_end(CPUState *cpu, uint32_t continuation)
{
    DcCallFrame *frame = dc_call_resume(cpu, continuation, DC_CALL_ANIMATION);
    if (frame->state == DC_CALL_COMPLETE) return;
    pt_anim_frame(cpu, frame->values[0], frame->values[1], frame->values[2]);
    dc_call_finish(frame);
}
