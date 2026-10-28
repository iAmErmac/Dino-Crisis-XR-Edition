#include "dc_continuation_wire.h"
static int wire_event_restore(const DcKernelEventState *state);
void pgxp_addr_reset(void);
#define dc_kernel_event_restore wire_event_restore
#define main dc_event_state_contract_main
#include "kernel_event_state_test.c"
#undef main
#undef dc_kernel_event_restore

static int wire_event_restore(const DcKernelEventState *state)
{
    uint32_t size = dc_continuation_wire_bytes();
    DcCallState *calls = calloc(1, sizeof *calls), *restored_calls = calloc(1, sizeof *restored_calls);
    DcKernelEventState *events = calloc(1, sizeof *events);
    uint8_t *bytes = malloc(size);
    assert(calls && restored_calls && events && bytes);
    dc_call_capture(calls);
    int ok = dc_continuation_wire_write(bytes, size, calls, state) &&
        dc_continuation_wire_read(bytes, size, restored_calls, events) &&
        dc_kernel_event_restore(events);
    free(calls); free(restored_calls); free(events); free(bytes);
    return ok;
}

static void test_wire(void)
{
    DcCallState *calls = calloc(1, sizeof *calls), *decoded = calloc(1, sizeof *decoded);
    DcKernelEventState *events = calloc(1, sizeof *events), *decoded_events = calloc(1, sizeof *decoded_events);
    assert(calls && decoded && events && decoded_events);
    calls->version = DC_CALL_STATE_VERSION; events->version = DC_KERNEL_EVENT_VERSION;
    DcCallFrame *frame = &calls->frames[63];
    frame->kind = DC_CALL_ANIMATION; frame->owner = 0xE29C; frame->stack = 0x801F0000;
    frame->continuation = 0x80018000; frame->state = DC_CALL_ACTIVE;
    frame->values[15] = 0x12345678;
    frame->pose.gsaved[1][2][15] = -1234;
    frame->mesh.cut[1] = UINT64_C(0x123456789ABCDEF0);
    frame->guest.words[33] = 0xAABBCCDD;
    frame->guest.generic.reg[33].scalar = -1.25;
    frame->guest.generic.reg[33].projection[3] = 2.75f;
    frame->guest.generic.reg[33].x16 = -65536;
    frame->guest.address.reg[31].x = -0.5f;
    frame->guest.address.reg[31].origin = 0x151200;
    frame->shot.depth = 12.5f; frame->shot.turn_error = -0.75f;
    DcKernelEventFrame *event_frame = &events->frames[31];
    event_frame->owner = 0xE35C; event_frame->stack = 0x801E0000;
    event_frame->continuation = 0x80018100; event_frame->waiting = 1; event_frame->hit = 1;
    event_frame->cls = 0xF0000011; event_frame->spec = 0x20; event_frame->index = 2;
    event_frame->saved.words[29] = event_frame->stack;
    event_frame->saved.words[31] = event_frame->continuation;
    event_frame->saved.generic.reg[33].scalar = -3.5;
    event_frame->saved.address.reg[31].origin = 0x152222;
    uint32_t size = dc_continuation_wire_bytes(); assert(size > 100000);
    uint8_t *wire = malloc(size), *wire2 = malloc(size); assert(wire && wire2);
    assert(dc_continuation_wire_write(wire, size, calls, events));
    assert(wire[0] == 1 && wire[1] == 0 && wire[4] == 64 && wire[8] == 32 &&
           wire[12] == DC_CALL_STATE_VERSION);
    assert(dc_continuation_wire_read(wire, size, decoded, decoded_events));
    assert(!memcmp(calls, decoded, sizeof *calls) && !memcmp(events, decoded_events, sizeof *events));
    assert(dc_continuation_wire_write(wire2, size, decoded, decoded_events));
    assert(!memcmp(wire, wire2, size));
    assert(dc_call_restore(decoded) && dc_kernel_event_restore(decoded_events));
    DcCallState *live = calloc(1, sizeof *live); assert(live);
    dc_call_capture(live); assert(!memcmp(calls, live, sizeof *calls));

    memset(decoded, 0xA5, sizeof *decoded); memset(decoded_events, 0x5A, sizeof *decoded_events);
    wire[12] = 7;
    assert(!dc_continuation_wire_read(wire, size, decoded, decoded_events));
    assert(((uint8_t *)decoded)[0] == 0xA5 && ((uint8_t *)decoded_events)[0] == 0x5A);
    wire[12] = 8;
    for (unsigned offset = 0; offset <= 12; offset += 4) {
        uint8_t byte = wire[offset]; wire[offset] ^= 0x80;
        assert(!dc_continuation_wire_read(wire, size, decoded, decoded_events));
        wire[offset] = byte;
        assert(((uint8_t *)decoded)[0] == 0xA5 && ((uint8_t *)decoded_events)[0] == 0x5A);
        dc_call_capture(live); assert(!memcmp(calls, live, sizeof *calls));
    }
    unsigned event_matches = 0;
    for (uint32_t i = 16; i + 4 <= size; ++i) {
        if (wire[i] != 0x5C || wire[i + 1] != 0xE3 || wire[i + 2] || wire[i + 3]) continue;
        ++event_matches; wire[i] = 0x5D;
        assert(!dc_continuation_wire_read(wire, size, decoded, decoded_events));
        wire[i] = 0x5C;
        assert(((uint8_t *)decoded)[0] == 0xA5 && ((uint8_t *)decoded_events)[0] == 0x5A);
    }
    assert(event_matches == 1);
    wire[16] = 0xFF;
    assert(!dc_continuation_wire_read(wire, size, decoded, decoded_events));
    wire[16] = 0;
    assert(!dc_continuation_wire_read(wire, size - 1, decoded, decoded_events));
    assert(!dc_continuation_wire_read(wire, size + 1, decoded, decoded_events));
    assert(!dc_continuation_wire_read(NULL, size, decoded, decoded_events));
    memset(wire2, 0xC3, size); calls->frames[63].owner = 3;
    assert(!dc_continuation_wire_write(wire2, size, calls, events));
    for (uint32_t i = 0; i < size; ++i) assert(wire2[i] == 0xC3);
    dc_call_reset(); dc_kernel_event_reset();
    free(wire); free(wire2); free(live); free(calls); free(decoded); free(events); free(decoded_events);
}

int main(void)
{
    dc_event_state_contract_main();
    test_wire();
    puts("PASS: portable continuation wire, last-slot fields and rejection without mutation");
    return 0;
}
