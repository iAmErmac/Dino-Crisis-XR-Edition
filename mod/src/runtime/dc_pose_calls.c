#include "dc_call_state.h"
extern void dc_ladder_draw_enter(CPUState *cpu);

void dc_pose_call_begin(CPUState *cpu, uint32_t continuation)
{
    DcCallFrame *frame = dc_call_begin(cpu, continuation, DC_CALL_DRAW_POSE);
    dc_ladder_draw_enter(cpu);
    dc_ladder_draw_capture(&frame->pose);
}

void dc_pose_call_end(CPUState *cpu, uint32_t continuation)
{
    DcCallFrame *frame = dc_call_resume(cpu, continuation, DC_CALL_DRAW_POSE);
    if (frame->state == DC_CALL_COMPLETE) return;
    dc_ladder_draw_restore(cpu, &frame->pose);
    dc_call_finish(frame);
}
