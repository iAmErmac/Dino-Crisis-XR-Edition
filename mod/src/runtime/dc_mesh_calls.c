#include "dc_call_state.h"
#include "pgxp_hooks.h"
#include <stdlib.h>
extern void psx_fatal_halt(const char *reason);

int dc_mesh_call_pending(CPUState *cpu, uint32_t continuation)
{
    DcCallFrame *frame = dc_call_lookup(cpu, continuation, DC_CALL_MESH);
    return frame && frame->state == DC_CALL_ACTIVE;
}

uint32_t dc_mesh_call_next(CPUState *cpu, uint32_t continuation)
{
    DcCallFrame *frame = dc_call_resume(cpu, continuation, DC_CALL_MESH);
    if (frame->state == DC_CALL_COMPLETE) return 0;
    while (frame->values[0] < 2) {
        unsigned phase = frame->values[0];
        uint32_t i = frame->values[1], n = frame->mesh.count[phase];
        uint64_t cut = frame->mesh.cut[phase];
        while (i < n && i < 64 && ((cut >> i) & 1u)) ++i;
        if (i == n) { ++frame->values[0]; frame->values[1] = 0; continue; }
        uint32_t j = i + 1;
        while (j < n && !(j < 64 && ((cut >> j) & 1u))) ++j;
        frame->values[1] = j;
        cpu->gpr[4] = frame->mesh.base[phase] + i * (phase ? 64u : 48u);
        cpu->gpr[5] = j - i;
        cpu->gpr[31] = frame->values[3 + phase * 2];
        psx_pgxp_forget_register(4); psx_pgxp_forget_register(5); psx_pgxp_forget_register(31);
        return frame->values[2 + phase * 2];
    }
    dc_call_finish(frame);
    return 0;
}

uint32_t dc_mesh_call_begin(CPUState *cpu, uint32_t header, uint32_t triangle_pc,
    uint32_t triangle_return, uint32_t quad_pc, uint32_t quad_return)
{
    if (!triangle_pc || !triangle_return || !quad_pc || !quad_return ||
        ((triangle_pc | triangle_return | quad_pc | quad_return) & 3u)) {
        psx_fatal_halt("attachment: invalid call targets"); abort();
    }
    DcCallFrame *frame = dc_call_begin(cpu, quad_return, DC_CALL_MESH);
    dc_meshrep_attach_runs(header, &frame->mesh);
    for (unsigned k = 0; k < 2; ++k) {
        if (frame->mesh.count[k] > 0xFFFFu || (frame->mesh.count[k] &&
            (uint64_t)frame->mesh.base[k] + frame->mesh.count[k] * (k ? 64ull : 48ull) > 0x100000000ull)) {
            psx_fatal_halt("attachment: invalid primitive span"); abort();
        }
    }
    frame->values[2] = triangle_pc; frame->values[3] = triangle_return;
    frame->values[4] = quad_pc; frame->values[5] = quad_return;
    return dc_mesh_call_next(cpu, quad_return);
}
