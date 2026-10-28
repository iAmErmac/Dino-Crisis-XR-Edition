#include "dc_call_state.h"
#include "pgxp_hooks.h"
#include <stdlib.h>
extern void psx_fatal_halt(const char *reason);

uint32_t dc_grenade_entry(CPUState *cpu, uint32_t continuation, uint32_t callee)
{
    DcCallFrame *frame = dc_call_lookup(cpu, continuation, DC_CALL_GRENADE);
    if (frame && frame->state == DC_CALL_ACTIVE) {
        dc_guest_registers_restore(cpu, &frame->guest);
        dc_grenade_complete(frame->values[0]);
        dc_call_finish(frame);
        return 0;
    }
    uint32_t object = dc_grenade_take();
    if (!object) return 0;
    if (!callee || (callee & 3u)) { psx_fatal_halt("grenade: invalid effect target"); abort(); }
    frame = dc_call_begin(cpu, continuation, DC_CALL_GRENADE);
    frame->values[0] = object;
    dc_guest_registers_capture(cpu, &frame->guest);
    cpu->gpr[4] = 0x80000000u | object; cpu->gpr[5] = cpu->gpr[6] = 1; cpu->gpr[31] = continuation;
    psx_pgxp_forget_register(4); psx_pgxp_forget_register(5);
    psx_pgxp_forget_register(6); psx_pgxp_forget_register(31);
    return callee;
}
