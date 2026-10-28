#include "dc_call_state.h"
#include "psx_scheduler.h"
#include <stdlib.h>
#include <string.h>

extern void psx_fatal_halt(const char *reason);
static DcCallState s_calls = {DC_CALL_STATE_VERSION};
static uint32_t s_active_kind_mask;

static uint32_t kind_bit(uint32_t kind)
{
    return kind > 0u && kind < 32u ? 1u << kind : 0u;
}

static void rebuild_active_kind_mask(void)
{
    s_active_kind_mask = 0;
    for (unsigned i = 0; i < DC_CALL_CAPACITY; ++i)
        if (s_calls.frames[i].state == DC_CALL_ACTIVE)
            s_active_kind_mask |= kind_bit(s_calls.frames[i].kind);
}

static _Noreturn void fail(const char *reason)
{
    psx_fatal_halt(reason);
    abort();
}

static uint32_t owner(CPUState *cpu)
{
    uint32_t tcb = psx_sched_current_tcb(cpu) & 0x1FFFFFFFu;
    if (!tcb || tcb >= 0x200000 || (tcb & 3))
        fail("call continuation: invalid current thread");
    return tcb;
}

static DcCallFrame *find(uint32_t tcb, uint32_t stack, uint32_t pc, uint32_t kind)
{
    for (unsigned i = 0; i < DC_CALL_CAPACITY; ++i) {
        DcCallFrame *frame = &s_calls.frames[i];
        if (frame->kind == kind && frame->owner == tcb &&
            frame->stack == stack && frame->continuation == pc) return frame;
    }
    return NULL;
}

int dc_call_has_kind(uint32_t kind)
{
    return (s_active_kind_mask & kind_bit(kind)) != 0;
}

DcCallFrame *dc_call_active(CPUState *cpu, uint32_t kind)
{
    uint32_t tcb = owner(cpu);
    for (unsigned i = 0; i < DC_CALL_CAPACITY; ++i)
        if (s_calls.frames[i].kind == kind && s_calls.frames[i].owner == tcb &&
            s_calls.frames[i].state == DC_CALL_ACTIVE) return &s_calls.frames[i];
    return NULL;
}

DcCallFrame *dc_call_begin(CPUState *cpu, uint32_t pc, uint32_t kind)
{
    uint32_t tcb = owner(cpu);
    if ((kind != DC_CALL_ANIMATION && kind != DC_CALL_BLOOD && kind != DC_CALL_DRAW_POSE && kind != DC_CALL_SOUND && kind != DC_CALL_MESH && kind != DC_CALL_GRENADE && kind != DC_CALL_SHOT && kind != DC_CALL_CREATURE_BLOOD && kind != DC_CALL_BACKGROUND) || !pc || (pc & 3) || !cpu->gpr[29])
        fail("call continuation: invalid call identity");
    DcCallFrame *existing = find(tcb, cpu->gpr[29], pc, kind);
    if (existing && existing->state == DC_CALL_ACTIVE)
        fail("call continuation: duplicate call entry");
    /* A new call cannot resume the thread's previously completed return. */
    for (unsigned i = 0; i < DC_CALL_CAPACITY; ++i)
        if (s_calls.frames[i].owner == tcb && s_calls.frames[i].state == DC_CALL_COMPLETE)
            memset(&s_calls.frames[i], 0, sizeof s_calls.frames[i]);
    for (unsigned i = 0; i < DC_CALL_CAPACITY; ++i) {
        DcCallFrame *frame = &s_calls.frames[i];
        if (frame->kind) continue;
        memset(frame, 0, sizeof *frame);
        frame->kind = kind; frame->owner = tcb;
        frame->stack = cpu->gpr[29]; frame->continuation = pc;
        frame->state = DC_CALL_ACTIVE;
        s_active_kind_mask |= kind_bit(kind);
        return frame;
    }
    fail("call continuation: capacity exceeded");
}

DcCallFrame *dc_call_lookup_stack(CPUState *cpu, uint32_t stack, uint32_t pc, uint32_t kind)
{
    return find(owner(cpu), stack, pc, kind);
}

DcCallFrame *dc_call_lookup(CPUState *cpu, uint32_t pc, uint32_t kind)
{
    return find(owner(cpu), cpu->gpr[29], pc, kind);
}

DcCallFrame *dc_call_resume(CPUState *cpu, uint32_t pc, uint32_t kind)
{
    DcCallFrame *frame = find(owner(cpu), cpu->gpr[29], pc, kind);
    if (!frame) fail("call continuation: return has no saved call");
    return frame;
}

void dc_call_finish(DcCallFrame *frame)
{
    for (unsigned i = 0; i < DC_CALL_CAPACITY; ++i)
        if (&s_calls.frames[i] != frame && s_calls.frames[i].owner == frame->owner &&
            s_calls.frames[i].state == DC_CALL_COMPLETE)
            memset(&s_calls.frames[i], 0, sizeof s_calls.frames[i]);
    memset(frame->values, 0, sizeof frame->values);
    memset(&frame->pose, 0, sizeof frame->pose);
    memset(&frame->sound, 0, sizeof frame->sound);
    memset(&frame->mesh, 0, sizeof frame->mesh);
    memset(&frame->guest, 0, sizeof frame->guest);
    memset(&frame->shot, 0, sizeof frame->shot);
    frame->state = DC_CALL_COMPLETE;
    rebuild_active_kind_mask();
}
void dc_call_clear(DcCallFrame *frame)
{
    memset(frame, 0, sizeof *frame);
    rebuild_active_kind_mask();
}
int dc_call_sound_active(void)
{
    for (unsigned i = 0; i < DC_CALL_CAPACITY; ++i)
        if (s_calls.frames[i].kind == DC_CALL_SOUND &&
            s_calls.frames[i].state == DC_CALL_ACTIVE && s_calls.frames[i].sound.page) return 1;
    return 0;
}
void dc_call_reset(void)
{
    for (unsigned i = 0; i < DC_CALL_CAPACITY; ++i)
        if (s_calls.frames[i].kind == DC_CALL_SOUND && s_calls.frames[i].state == DC_CALL_ACTIVE)
            dc_sound_swap_restore(&s_calls.frames[i].sound);
    memset(&s_calls, 0, sizeof s_calls);
    s_calls.version = DC_CALL_STATE_VERSION;
    s_active_kind_mask = 0;
}
void dc_call_discard_thread(uint32_t tcb)
{
    tcb &= 0x1FFFFFFFu;
    for (unsigned i = 0; i < DC_CALL_CAPACITY; ++i)
        if (s_calls.frames[i].kind && s_calls.frames[i].owner == tcb) {
            if (s_calls.frames[i].kind == DC_CALL_SOUND && s_calls.frames[i].state == DC_CALL_ACTIVE)
                dc_sound_swap_restore(&s_calls.frames[i].sound);
            memset(&s_calls.frames[i], 0, sizeof s_calls.frames[i]);
        }
    rebuild_active_kind_mask();
}
void dc_call_capture(DcCallState *state) { *state = s_calls; }
int dc_call_validate(const DcCallState *state)
{
    if (!state || state->version != DC_CALL_STATE_VERSION) return 0;
    uint32_t sound_page = 0;
    for (unsigned i = 0; i < DC_CALL_CAPACITY; ++i) {
        const DcCallFrame *frame = &state->frames[i];
        if (!frame->kind) { if (frame->state != 0) return 0; continue; }
        if ((frame->kind != DC_CALL_ANIMATION && frame->kind != DC_CALL_BLOOD && frame->kind != DC_CALL_DRAW_POSE && frame->kind != DC_CALL_SOUND && frame->kind != DC_CALL_MESH && frame->kind != DC_CALL_GRENADE && frame->kind != DC_CALL_SHOT && frame->kind != DC_CALL_CREATURE_BLOOD && frame->kind != DC_CALL_BACKGROUND) || !frame->owner ||
            frame->owner >= 0x200000 || (frame->owner & 3) || !frame->stack ||
            !frame->continuation || (frame->continuation & 3) ||
            (frame->state != DC_CALL_ACTIVE && frame->state != DC_CALL_COMPLETE)) return 0;
        if (frame->state == DC_CALL_ACTIVE && frame->kind == DC_CALL_BLOOD &&
            (frame->values[1] != frame->continuation || frame->values[2] > 0xFFFFu ||
             frame->values[3] > 0x7FFFFFFFu || frame->values[4] > frame->values[3])) return 0;
        if (frame->kind == DC_CALL_DRAW_POSE &&
            (frame->pose.applied > 1 || frame->pose.papplied > 1 ||
             frame->pose.gapplied > 1 || frame->pose.rapplied > 1 ||
             frame->pose.t0applied > 1 || frame->pose.ladder_applied > 1)) return 0;
        if (frame->kind == DC_CALL_SOUND) {
            if (!dc_sound_swap_valid(&frame->sound) ||
                (frame->state == DC_CALL_COMPLETE && frame->sound.page)) return 0;
            if (frame->sound.page) {
                if (sound_page) return 0;
                sound_page = frame->sound.page;
            }
        }
        if (frame->kind == DC_CALL_MESH && frame->state == DC_CALL_ACTIVE) {
            if (frame->values[0] > 1 || frame->values[1] > frame->mesh.count[frame->values[0]] ||
                frame->values[5] != frame->continuation) return 0;
            for (unsigned k = 2; k < 6; ++k)
                if (!frame->values[k] || (frame->values[k] & 3u)) return 0;
            for (unsigned k = 0; k < 2; ++k)
                if (frame->mesh.count[k] > 0xFFFFu || (frame->mesh.count[k] &&
                    (uint64_t)frame->mesh.base[k] + frame->mesh.count[k] * (k ? 64ull : 48ull) > 0x100000000ull)) return 0;
        }
        if (frame->kind == DC_CALL_GRENADE && frame->state == DC_CALL_ACTIVE) {
            if (!frame->values[0] || frame->values[0] + 61ull > 0x200000u ||
                frame->guest.words[29] != frame->stack) return 0;
            for (unsigned k = 1; k < 34; ++k)
                if (frame->guest.generic.reg[k].live > 1 || frame->guest.generic.reg[k].scalar_live > 1 ||
                    (frame->guest.generic.reg[k].flags & ~0x7Fu)) return 0;
        }
        if (frame->kind == DC_CALL_SHOT && frame->state == DC_CALL_ACTIVE) {
            if (frame->values[2] > 2 || !frame->values[8] || (frame->values[8] & 3u) ||
                frame->stack < frame->values[8] || !frame->values[9] || !frame->values[10] ||
                ((frame->values[9] | frame->values[10]) & 3u)) return 0;
            if (frame->values[2]) {
                if ((uint64_t)frame->guest.words[29] + frame->values[8] != frame->stack ||
                    frame->shot.enabled != 1 || frame->shot.visibility > 1 ||
                    frame->shot.count < 0 || frame->shot.count > 3 ||
                    frame->shot.victim_count < 0 || frame->shot.victim_count > 3 ||
                    frame->values[3] > (uint32_t)frame->shot.victim_count ||
                    frame->values[4] > 0x7FFFFFFFu || frame->values[5] > frame->values[4] ||
                    frame->values[6] > 0xFFFFu) return 0;
                for (unsigned k = 1; k < 34; ++k)
                    if (frame->guest.generic.reg[k].live > 1 || frame->guest.generic.reg[k].scalar_live > 1 ||
                        (frame->guest.generic.reg[k].flags & ~0x7Fu)) return 0;
            }
        }
        if (frame->kind == DC_CALL_CREATURE_BLOOD) {
            uint32_t phys = frame->values[0] & 0x1FFFFFFFu;
            if (frame->state != DC_CALL_ACTIVE || frame->values[7] != 1 ||
                frame->values[1] > 0xFFFFu || frame->values[3] > 0xFFFFu ||
                !frame->values[4] || frame->values[4] > 0x7FFFFFFFu ||
                frame->values[5] >= frame->values[4] || !frame->values[6] ||
                (frame->values[6] & 3u) || frame->guest.pc != frame->values[6] ||
                frame->guest.words[29] != frame->stack ||
                phys != frame->values[2] + 280ull || frame->values[2] < 0xB7ACCu ||
                frame->values[2] >= 0xB7ACCu + 10u * 608u ||
                (frame->values[2] - 0xB7ACCu) % 608u || (frame->values[8] >> 26) != 0x29u)
                return 0;
            for (unsigned k = 1; k < 34; ++k)
                if (frame->guest.generic.reg[k].live > 1 || frame->guest.generic.reg[k].scalar_live > 1 ||
                    (frame->guest.generic.reg[k].flags & ~0x7Fu)) return 0;
        }
        if (frame->kind == DC_CALL_BACKGROUND) {
            if (frame->state != DC_CALL_ACTIVE || frame->continuation != 0x8008F6F4u ||
                frame->values[0] < 1 || frame->values[0] > 2) return 0;
            if (frame->values[0] == 2) {
                if (frame->guest.pc != frame->continuation ||
                    frame->guest.words[29] != frame->stack ||
                    frame->guest.words[31] != 0x8002730Cu) return 0;
                for (unsigned k = 1; k < 34; ++k)
                    if (frame->guest.generic.reg[k].live > 1 || frame->guest.generic.reg[k].scalar_live > 1 ||
                        (frame->guest.generic.reg[k].flags & ~0x7Fu)) return 0;
            }
        }
        for (unsigned j = 0; j < i; ++j) {
            const DcCallFrame *other = &state->frames[j];
            if ((frame->kind == DC_CALL_CREATURE_BLOOD || frame->kind == DC_CALL_BACKGROUND) && other->kind == frame->kind &&
                frame->owner == other->owner) return 0;
            if (frame->state == DC_CALL_COMPLETE && other->state == DC_CALL_COMPLETE &&
                frame->owner == other->owner) return 0;
            if (frame->kind == other->kind && frame->owner == other->owner &&
                frame->stack == other->stack && frame->continuation == other->continuation)
                return 0;
        }
    }
    return 1;
}
int dc_call_restore(const DcCallState *state)
{
    if (!dc_call_validate(state)) return 0;
    uint32_t sound_page = 0;
    for (unsigned i = 0; i < DC_CALL_CAPACITY; ++i)
        if (state->frames[i].kind == DC_CALL_SOUND && state->frames[i].sound.page)
            sound_page = state->frames[i].sound.page;
    if (sound_page || dc_call_sound_active()) dc_sound_capture_page(sound_page);
    s_calls = *state;
    rebuild_active_kind_mask();
    return 1;
}
