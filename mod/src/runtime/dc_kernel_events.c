#include "dc_kernel_events.h"
#include "dc_kernel.h"
#include "psx_scheduler.h"
#include "pgxp_hooks.h"
#include <stdlib.h>
#include <string.h>

extern void psx_fatal_halt(const char *reason);
extern void dc_kernel_event_result(int hit);
extern void dc_kernel_card_arm(uint32_t delay, uint32_t spec);
static DcKernelEventState s_events = {DC_KERNEL_EVENT_VERSION};
static uint32_t s_active;
enum { EV_STRIDE = 0x1C, EV_ENABLED = 0x2000, EV_DELIVERED = 0x4000 };

static _Noreturn void fail(const char *reason) { psx_fatal_halt(reason); abort(); }
static uint32_t owner(CPUState *cpu)
{
    return psx_sched_current_tcb(cpu) & 0x1FFFFFFFu;
}
static uint32_t top(uint32_t tcb)
{
    uint32_t result = 0;
    for (unsigned i = 0; i < DC_KERNEL_EVENT_CAPACITY; ++i) {
        if (s_events.frames[i].owner != tcb) continue;
        int has_child = 0;
        for (unsigned j = 0; j < DC_KERNEL_EVENT_CAPACITY; ++j)
            if (s_events.frames[j].owner && s_events.frames[j].parent == i + 1) has_child = 1;
        if (!has_child) {
            if (result) fail("event: ambiguous service stack");
            result = i + 1;
        }
    }
    return result;
}
static void advance(CPUState *cpu, DcKernelEventFrame *frame)
{
    if (frame->waiting) {
        dc_guest_registers_restore_mask(cpu, &frame->saved, (1u << 4) | (1u << 5) | (1u << 31));
        frame->waiting = 0;
    }
    for (;;) {
        while (frame->index < HLE_EVCB_SIZE / EV_STRIDE) {
            uint32_t event = 0x80000000u | (HLE_EVCB_ADDR + frame->index++ * EV_STRIDE);
            if (cpu->read_word(event + 4) != EV_ENABLED || cpu->read_word(event) != frame->cls ||
                cpu->read_word(event + 8) != frame->spec) continue;
            uint32_t mode = cpu->read_word(event + 12);
            if (mode == 0x2000) {
                frame->hit = 1;
                cpu->write_word(event + 4, EV_DELIVERED);
            } else if (mode == 0x1000) {
                uint32_t function = cpu->read_word(event + 16);
                if (!function) continue;
                if (function & 3u) fail("event: unaligned guest callback");
                frame->hit = 1; frame->waiting = 1;
                cpu->gpr[4] = frame->cls; cpu->gpr[5] = frame->spec;
                cpu->gpr[31] = frame->continuation;
                psx_pgxp_forget_register(4); psx_pgxp_forget_register(5); psx_pgxp_forget_register(31);
                cpu->pc = function;
                return;
            }
        }
        dc_kernel_event_result(frame->hit != 0);
        if (frame->kind == 1 && frame->class_index == 0) {
            frame->class_index = 1; frame->cls = 0xF4000001u;
            frame->index = frame->hit = 0;
            continue;
        }
        uint32_t continuation = frame->continuation;
        uint32_t result = frame->kind ? frame->result : 0;
        uint32_t arm_delay = frame->arm_delay, arm_spec = frame->arm_spec;
        memset(frame, 0, sizeof *frame);
        --s_active;
        if (arm_delay) dc_kernel_card_arm(arm_delay, arm_spec);
        cpu->gpr[2] = result; psx_pgxp_forget_register(2);
        cpu->pc = continuation;
        return;
    }
}

static DcKernelEventFrame *push(CPUState *cpu)
{
    uint32_t tcb = owner(cpu);
    if (!tcb || tcb >= 0x200000u || (tcb & 3u) || !cpu->gpr[29] ||
        !cpu->gpr[31] || (cpu->gpr[31] & 3u)) fail("event: invalid service identity");
    uint32_t parent = top(tcb);
    for (unsigned i = 0; i < DC_KERNEL_EVENT_CAPACITY; ++i) {
        DcKernelEventFrame *frame = &s_events.frames[i];
        if (frame->owner) continue;
        memset(frame, 0, sizeof *frame);
        frame->owner = tcb; frame->parent = parent;
        ++s_active;
        frame->stack = cpu->gpr[29]; frame->continuation = cpu->gpr[31];
        dc_guest_registers_capture(cpu, &frame->saved);
        return frame;
    }
    fail("event: service stack capacity exceeded");
}
void dc_kernel_event_begin(CPUState *cpu, uint32_t cls, uint32_t spec)
{
    DcKernelEventFrame *frame = push(cpu);
    frame->cls = cls; frame->spec = spec;
    advance(cpu, frame);
}
void dc_kernel_event_defer_card(CPUState *cpu, uint32_t spec, uint32_t arm_delay, uint32_t arm_spec)
{
    uint32_t parent = top(owner(cpu));
    if (parent && !s_events.frames[parent - 1].waiting) fail("card event: native completion already queued");
    if (arm_delay > 0x7FFFFFFFu) fail("card event: invalid completion delay");
    DcKernelEventFrame *frame = push(cpu);
    frame->kind = 1; frame->cls = 0xF0000011u; frame->spec = spec;
    frame->arm_delay = arm_delay; frame->arm_spec = arm_spec;
}
int dc_kernel_event_complete_native(CPUState *cpu)
{
    if (!s_active) return 0;
    uint32_t index = top(owner(cpu));
    if (!index) return 0;
    DcKernelEventFrame *frame = &s_events.frames[index - 1];
    if (frame->waiting || frame->kind != 1 || frame->stack != cpu->gpr[29]) return 0;
    frame->result = cpu->gpr[2];
    dc_guest_registers_restore_mask(cpu, &frame->saved, 1u << 2);
    advance(cpu, frame);
    return 1;
}
int dc_kernel_event_pending(CPUState *cpu, uint32_t pc)
{
    if (!s_active || !cpu->read_word) return 0;
    uint32_t tcb = owner(cpu);
    if (!tcb || tcb >= 0x200000u || (tcb & 3u)) return 0;
    uint32_t index = top(tcb);
    if (!index) return 0;
    DcKernelEventFrame *frame = &s_events.frames[index - 1];
    return frame->waiting && frame->stack == cpu->gpr[29] && frame->continuation == pc;
}
int dc_kernel_event_resume(CPUState *cpu, uint32_t pc)
{
    if (!dc_kernel_event_pending(cpu, pc)) return 0;
    advance(cpu, &s_events.frames[top(owner(cpu)) - 1]);
    return 1;
}
void dc_kernel_event_discard_thread(uint32_t tcb)
{
    tcb &= 0x1FFFFFFFu;
    if (!tcb) return;
    for (unsigned i = 0; i < DC_KERNEL_EVENT_CAPACITY; ++i)
        if (s_events.frames[i].owner == tcb) {
            memset(&s_events.frames[i], 0, sizeof s_events.frames[i]);
            --s_active;
        }
}
void dc_kernel_event_reset(void)
{
    memset(&s_events, 0, sizeof s_events); s_events.version = DC_KERNEL_EVENT_VERSION;
    s_active = 0;
}
void dc_kernel_event_capture(DcKernelEventState *state) { *state = s_events; }
int dc_kernel_event_validate(const DcKernelEventState *state)
{
    if (!state || state->version != DC_KERNEL_EVENT_VERSION) return 0;
    for (unsigned i = 0; i < DC_KERNEL_EVENT_CAPACITY; ++i) {
        const DcKernelEventFrame *frame = &state->frames[i];
        if (!frame->owner) {
            DcKernelEventFrame empty = {0};
            if (memcmp(frame, &empty, sizeof empty)) return 0;
            continue;
        }
        if (frame->owner >= 0x200000u || (frame->owner & 3u) || !frame->stack ||
            !frame->continuation || (frame->continuation & 3u) || frame->waiting != 1 ||
            frame->hit != 1 || frame->kind > 1 || frame->class_index > frame->kind ||
            frame->arm_delay > 0x7FFFFFFFu || (!frame->kind && (frame->class_index || frame->arm_delay || frame->arm_spec || frame->result)) ||
            (frame->kind && frame->cls != (frame->class_index ? 0xF4000001u : 0xF0000011u)) ||
            frame->index > HLE_EVCB_SIZE / EV_STRIDE ||
            frame->saved.words[29] != frame->stack || frame->saved.words[31] != frame->continuation ||
            frame->parent > DC_KERNEL_EVENT_CAPACITY || frame->parent == i + 1) return 0;
        if (frame->parent && state->frames[frame->parent - 1].owner != frame->owner) return 0;
        for (unsigned k = 1; k < 34; ++k)
            if (frame->saved.generic.reg[k].live > 1 || frame->saved.generic.reg[k].scalar_live > 1 ||
                (frame->saved.generic.reg[k].flags & ~0x7Fu)) return 0;
        unsigned depth = 0; uint32_t parent = frame->parent;
        while (parent) {
            if (++depth > DC_KERNEL_EVENT_CAPACITY) return 0;
            parent = state->frames[parent - 1].parent;
            if (parent > DC_KERNEL_EVENT_CAPACITY) return 0;
        }
        for (unsigned j = 0; j < i; ++j)
            if (state->frames[j].owner == frame->owner && state->frames[j].parent == frame->parent) return 0;
    }
    return 1;
}
int dc_kernel_event_restore(const DcKernelEventState *state)
{
    if (!dc_kernel_event_validate(state)) return 0;
    s_events = *state;
    s_active = 0;
    for (unsigned i = 0; i < DC_KERNEL_EVENT_CAPACITY; ++i)
        if (state->frames[i].owner) ++s_active;
    return 1;
}
