#include "dc_continuation_wire.h"
#include "pst_wire.h"
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(float) == 4 && sizeof(double) == 8, "Continuation wire requires 32/64-bit floating point");

/* Every field is encoded separately; host padding never enters the wire. */
typedef struct DcWire { PstW out; PstR in; int reading, ok; } DcWire;
static void field(DcWire *w, void *data, size_t bytes, size_t unit)
{
    if (!w->ok) return;
    w->ok = w->reading ? pst_r_pod(&w->in, data, bytes, unit) :
        pst_w_pod(&w->out, data, bytes, unit);
}
#define F(member) field(w, &value->member, sizeof value->member, sizeof value->member)
#define A(member, unit) field(w, &value->member, sizeof value->member, unit)

static void cell(DcWire *w, PxCell *value)
{
    F(x);
    F(y);
    F(z);
    F(raw);
    F(bits);
    F(origin);
}

static void shadow(DcWire *w, PsxRegisterShadow *value)
{
    F(value);
    F(live);
    F(scalar_live);
    F(x16);
    F(y16);
    F(z);
    F(flags);
    A(projection, 4);
    F(scalar);
}

static void guest(DcWire *w, DcGuestRegisters *value)
{
    A(words, 4);
    F(pc);
    F(generic.generation);
    for (unsigned i = 0; i < 34; ++i) shadow(w, &value->generic.reg[i]);
    F(address.generation);
    A(address.value, 4);
    A(address.source, 4);
    for (unsigned i = 0; i < 32; ++i) cell(w, &value->address.reg[i]);
}

static void pose(DcWire *w, DcDrawPose *value)
{
    F(applied);
    F(papplied);
    F(gapplied);
    F(rapplied);
    F(t0applied);
    F(ladder_applied);
    A(saved, 2);
    A(wrote, 2);
    A(psaved, 2);
    A(pwrote, 2);
    A(gsaved, 2);
    A(gwrote, 2);
    A(rsaved, 2);
    A(rwrote, 2);
    A(t0saved, 2);
    A(t0wrote, 2);
    A(ladder_saved, 2);
    A(ladder_wrote, 2);
}

static void shot(DcWire *w, DcShotState *value)
{
    F(enabled);
    F(visibility);
    F(actor);
    F(count);
    A(original, 4);
    F(game_hit);
    F(candidate);
    F(hit);
    F(turning);
    A(victims, 4);
    F(victim_count);
    F(blood_event);
    F(depth);
    F(turn_error);
}

static void call(DcWire *w, DcCallFrame *value)
{
    F(kind);
    F(owner);
    F(stack);
    F(continuation);
    F(state);
    A(values, 4);
    pose(w, &value->pose);
    F(sound.entry_address);
    F(sound.table_address);
    F(sound.entry_word);
    F(sound.table_word);
    F(sound.page);
    A(mesh.base, 4);
    A(mesh.count, 4);
    A(mesh.cut, 8);
    guest(w, &value->guest);
    shot(w, &value->shot);
}

static void event(DcWire *w, DcKernelEventFrame *value)
{
    F(owner);
    F(parent);
    F(stack);
    F(continuation);
    F(cls);
    F(spec);
    F(index);
    F(hit);
    F(waiting);
    F(kind);
    F(class_index);
    F(result);
    F(arm_delay);
    F(arm_spec);
    guest(w, &value->saved);
}

static void states(DcWire *w, DcCallState *calls, DcKernelEventState *events)
{
    uint32_t version = 1, call_capacity = DC_CALL_CAPACITY, event_capacity = DC_KERNEL_EVENT_CAPACITY;
    field(w, &version, 4, 4);
    field(w, &call_capacity, 4, 4);
    field(w, &event_capacity, 4, 4);
    if (version != 1 || call_capacity != DC_CALL_CAPACITY || event_capacity != DC_KERNEL_EVENT_CAPACITY) {
        w->ok = 0; return;
    }
    field(w, &calls->version, 4, 4);
    for (unsigned i = 0; i < DC_CALL_CAPACITY; ++i) call(w, &calls->frames[i]);
    field(w, &events->version, 4, 4);
    for (unsigned i = 0; i < DC_KERNEL_EVENT_CAPACITY; ++i) event(w, &events->frames[i]);
}

typedef struct DcContinuationState { DcCallState calls; DcKernelEventState events; } DcContinuationState;

uint32_t dc_continuation_wire_bytes(void)
{
    DcContinuationState *state = calloc(1, sizeof *state);
    if (!state) return 0;
    DcWire wire = {0}; wire.ok = 1;
    pst_w_init(&wire.out, NULL, 0);
    states(&wire, &state->calls, &state->events);
    uint32_t result = wire.ok && wire.out.written <= UINT32_MAX ? (uint32_t)wire.out.written : 0;
    free(state);
    return result;
}

int dc_continuation_wire_write(uint8_t *data, uint32_t size,
    const DcCallState *calls, const DcKernelEventState *events)
{
    uint32_t expected = dc_continuation_wire_bytes();
    if (!data || !expected || size != expected || !dc_call_validate(calls) || !dc_kernel_event_validate(events)) return 0;
    DcContinuationState *state = malloc(sizeof *state);
    if (!state) return 0;
    state->calls = *calls; state->events = *events;
    DcWire wire = {0}; wire.ok = 1;
    pst_w_init(&wire.out, data, size);
    states(&wire, &state->calls, &state->events);
    int ok = wire.ok && wire.out.written == size;
    free(state);
    return ok;
}

int dc_continuation_wire_read(const uint8_t *data, uint32_t size,
    DcCallState *calls, DcKernelEventState *events)
{
    uint32_t expected = dc_continuation_wire_bytes();
    if (!data || !calls || !events || !expected || size != expected) return 0;
    DcContinuationState *state = calloc(1, sizeof *state);
    if (!state) return 0;
    DcWire wire = {0}; wire.ok = 1; wire.reading = 1;
    pst_r_init(&wire.in, data, size);
    states(&wire, &state->calls, &state->events);
    int ok = wire.ok && wire.in.p == wire.in.end &&
        dc_call_validate(&state->calls) && dc_kernel_event_validate(&state->events);
    if (ok) { *calls = state->calls; *events = state->events; }
    free(state);
    return ok;
}
