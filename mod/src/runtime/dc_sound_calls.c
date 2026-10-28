#include "dc_call_state.h"
extern void dc_shot_effect_note(uint32_t actor);

void dc_sound_call_begin(CPUState *cpu)
{
    int busy = dc_call_sound_active();
    DcCallFrame *frame = dc_call_begin(cpu, cpu->gpr[31], DC_CALL_SOUND);
    if (!busy) dc_fk_sound_prepare(cpu, &frame->sound);
    dc_shot_effect_note(cpu->gpr[4]);
}

void dc_sound_call_end(CPUState *cpu, uint32_t continuation)
{
    DcCallFrame *frame = dc_call_resume(cpu, continuation, DC_CALL_SOUND);
    if (frame->state == DC_CALL_COMPLETE) return;
    dc_sound_swap_restore(&frame->sound);
    dc_call_finish(frame);
}
