#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "dc_background.h"
#include "dc_call_state.h"
#include "dc_continuation_wire.h"
#include "dc_guest_registers.h"
#include "dc_kernel.h"
#include "dc_kernel_events.h"
#include "dc_pgxp.h"
#include "pgxp.h"
#include "pgxp_cells.h"
#include "pgxp_hooks.h"

enum {
    RAM_SIZE = 0x200000,
    TCB_A = 0xA000E29Cu,
    TCB_B = 0xA000E35Cu,
    STACK_A = 0x801F0000u,
    STACK_B = 0x801EF000u,
    HANDLER = 0x8002731Cu,
    HANDLER_RA = 0x800272B8u,
    RETURN = 0x8008F6F4u,
    RETURN_RA = 0x8002730Cu,
    BG_A = 0x800B79C0u,
    BG_MAX = 0x801FFF8Cu,
    EVENT_CLASS = 0xF1000010u,
    EVENT_SPEC = 0x0010u,
    EVENT_CALLBACK = 0x80018000u,
};

static uint8_t ram[RAM_SIZE];
static uint32_t current_tcb;
static int camera_active;
static int scripted;
static int scene_flag;

static size_t ram_offset(uint32_t address)
{
    return (size_t)(address - 0x80000000u);
}

static uint32_t read_word(uint32_t address)
{
    uint32_t value;
    memcpy(&value, &ram[ram_offset(address)], sizeof value);
    return value;
}

static void write_word(uint32_t address, uint32_t value)
{
    memcpy(&ram[ram_offset(address)], &value, sizeof value);
}

static uint32_t event_address(unsigned index)
{
    assert(index < HLE_EVCB_SIZE / 0x1Cu);
    return 0x80000000u + HLE_EVCB_ADDR + index * 0x1Cu;
}

static void seed_event(CPUState *cpu)
{
    uint32_t address = event_address(0);
    write_word(address + 0u, EVENT_CLASS);
    write_word(address + 4u, 0x2000u);
    write_word(address + 8u, EVENT_SPEC);
    write_word(address + 12u, 0x1000u);
    write_word(address + 16u, EVENT_CALLBACK);
    dc_kernel_event_begin(cpu, EVENT_CLASS, EVENT_SPEC);
    assert(cpu->pc == EVENT_CALLBACK);
}

static void set_byte(uint32_t address, uint8_t value)
{
    assert(ram_offset(address) < RAM_SIZE);
    ram[ram_offset(address)] = value;
}

static void set_pending(uint32_t background, int pending)
{
    size_t offset = ram_offset(background);
    assert(offset + 115u < RAM_SIZE);
    ram[offset + 114u] = (uint8_t)((ram[offset + 114u] & ~1u) | (pending ? 1u : 0u));
}

uint8_t *memory_get_ram_ptr(void) { return ram; }
uint32_t psx_sched_current_tcb(CPUState *cpu) { (void)cpu; return current_tcb; }
int dc_nots_active(void) { return camera_active; }
int dc_scripted_now(void) { return scripted; }
int dc_scene_flag_on(void) { return scene_flag; }
void spu_xpage_capture(int page) { (void)page; }
void psx_fatal_halt(const char *reason) { (void)reason; abort(); }
void dc_kernel_event_result(int hit) { (void)hit; abort(); }
void dc_kernel_card_arm(uint32_t delay, uint32_t spec)
{
    (void)delay; (void)spec; abort();
}

static void seed_register_precision(unsigned reg, uint32_t raw,
                                    uint32_t origin, uint32_t source,
                                    int32_t x16, int32_t y16, uint16_t depth)
{
    uint32_t opcode = (0x32u << 26);
    dc_pgxp_cop2(opcode, raw, origin);
    dc_pgxp_project(0, x16, y16, depth, raw, 1);
    pgxp_gte_push_sxy(x16, y16, depth, raw);
    psx_pgxp_cop2(NULL, (0x3Au << 26) | (14u << 11), raw, source);
    psx_pgxp_load(NULL, (0x23u << 26) | (reg << 16), source, raw);
}

static void cpu_init(CPUState *cpu)
{
    memset(cpu, 0, sizeof *cpu);
    cpu->read_word = read_word;
    cpu->write_word = write_word;
    for (unsigned i = 0; i < 32; ++i) cpu->gpr[i] = 0x11000000u + i * 0x01010101u;
    cpu->gpr[0] = 0;
    cpu->gpr[29] = STACK_A;
    cpu->gpr[31] = HANDLER_RA;
    cpu->hi = 0x12345678u;
    cpu->lo = 0x9ABCDEF0u;
}

static void reset_fixture(CPUState *cpu)
{
    memset(ram, 0, sizeof ram);
    dc_call_reset();
    dc_kernel_event_reset();
    current_tcb = TCB_A;
    camera_active = 1;
    scripted = 0;
    scene_flag = 0;
    pgxp_invalidate_all();
    pgxp_addr_reset();
    cpu_init(cpu);
}

static void arm(CPUState *cpu, uint32_t background)
{
    cpu->gpr[4] = background;
    cpu->gpr[31] = HANDLER_RA;
    assert(dc_background_enter(cpu, HANDLER) == 0);
}

static int trigger(CPUState *cpu, uint32_t background)
{
    cpu->gpr[4] = 0x80012340u;
    cpu->gpr[5] = background;
    cpu->gpr[31] = RETURN_RA;
    cpu->pc = 0;
    return dc_background_enter(cpu, RETURN);
}

static const DcCallFrame *background_frame(const DcCallState *state)
{
    for (unsigned i = 0; i < DC_CALL_CAPACITY; ++i)
        if (state->frames[i].kind == DC_CALL_BACKGROUND) return &state->frames[i];
    return NULL;
}

static void test_contract_identity_and_arm(void)
{
    CPUState cpu;
    DcCallState state;
    reset_fixture(&cpu);

    assert(DC_CALL_STATE_VERSION == 9 && DC_CALL_BACKGROUND == 9);
    arm(&cpu, BG_A);
    dc_call_capture(&state);
    assert(dc_call_validate(&state));
    const DcCallFrame *frame = background_frame(&state);
    assert(frame && frame->state == DC_CALL_ACTIVE && frame->owner == (TCB_A & 0x1FFFFFFFu));
    assert(frame->stack == STACK_A && frame->continuation == RETURN);
    assert(!dc_background_pending(&cpu, RETURN));
}

static void test_running_restore_and_call_state_roundtrip(void)
{
    CPUState cpu, expected;
    DcCallState state;
    reset_fixture(&cpu);
    arm(&cpu, BG_A);
    set_pending(BG_A, 1);
    cpu.gpr[4] = 0x80012340u;
    cpu.gpr[5] = BG_A;
    cpu.gpr[31] = RETURN_RA;
    cpu.pc = 0;
    expected = cpu;
    assert(dc_background_enter(&cpu, RETURN));
    assert(cpu.pc == HANDLER && cpu.gpr[4] == BG_A && cpu.gpr[5] == BG_A);
    assert(cpu.gpr[31] == RETURN);
    assert(dc_background_pending(&cpu, RETURN));

    dc_call_capture(&state);
    assert(dc_call_validate(&state));
    dc_call_reset();
    assert(!dc_background_pending(&cpu, RETURN));
    assert(dc_call_restore(&state));
    assert(dc_background_pending(&cpu, RETURN));

    set_byte(BG_A + 114u, 0);
    set_byte(BG_A + 115u, 7);
    set_byte(0x800B92C8u, 1);
    for (unsigned i = 1; i < 32; ++i)
        if (i != 29) cpu.gpr[i] ^= 0x55AA0000u;
    cpu.gpr[4] = 0xDEAD0004u;
    cpu.gpr[31] = 0xDEAD0031u;
    cpu.hi = 0xDEAD1111u;
    cpu.lo = 0xDEAD2222u;
    cpu.pc = HANDLER;
    dc_background_resume(&cpu, RETURN);

    assert(memcmp(cpu.gpr, expected.gpr, sizeof cpu.gpr) == 0);
    assert(cpu.hi == expected.hi && cpu.lo == expected.lo && cpu.pc == RETURN);
    assert(ram[ram_offset(BG_A) + 114u] == 0);
    assert(ram[ram_offset(BG_A) + 115u] == 7);
    assert(ram[ram_offset(0x800B92C8u)] == 1);
    assert(!dc_background_pending(&cpu, RETURN));
    assert(!dc_call_has_kind(DC_CALL_BACKGROUND));
    dc_call_capture(&state);
    assert(background_frame(&state) == NULL);
}

static void wire_roundtrip(const DcCallState *calls,
                           const DcKernelEventState *events,
                           DcCallState *decoded_calls,
                           DcKernelEventState *decoded_events)
{
    uint32_t size = dc_continuation_wire_bytes();
    uint8_t *wire = (uint8_t *)malloc(size);
    uint8_t *wire2 = (uint8_t *)malloc(size);
    assert(size > 100000 && wire && wire2);
    assert(dc_continuation_wire_write(wire, size, calls, events));
    assert(dc_continuation_wire_read(wire, size, decoded_calls, decoded_events));
    assert(!memcmp(calls, decoded_calls, sizeof *calls));
    assert(!memcmp(events, decoded_events, sizeof *events));
    assert(dc_continuation_wire_write(wire2, size, decoded_calls, decoded_events));
    assert(!memcmp(wire, wire2, size));
    free(wire2);
    free(wire);
}

static void assert_wire_write_rejects(const DcCallState *calls,
                                      const DcKernelEventState *events,
                                      unsigned malformed)
{
    DcCallState bad = *calls;
    DcCallFrame *frame = (DcCallFrame *)background_frame(&bad);
    uint32_t size = dc_continuation_wire_bytes();
    uint8_t *wire = (uint8_t *)malloc(size);
    assert(frame && wire);
    switch (malformed) {
    case 0: frame->values[0] = 3; break;
    case 1: frame->continuation = RETURN - 4u; break;
    case 2: frame->owner = 3; break;
    case 3: frame->guest.generic.reg[6].live = 2; break;
    default: abort();
    }
    memset(wire, 0xC3, size);
    assert(!dc_continuation_wire_write(wire, size, &bad, events));
    for (uint32_t i = 0; i < size; ++i) assert(wire[i] == 0xC3);
    free(wire);
}

static void test_continuation_wire_armed_and_running(void)
{
    CPUState cpu;
    DcCallState calls, decoded_calls;
    DcKernelEventState events, decoded_events;

    reset_fixture(&cpu);
    seed_event(&cpu);
    arm(&cpu, BG_A);
    dc_call_capture(&calls);
    dc_kernel_event_capture(&events);
    assert(dc_call_validate(&calls));
    assert(dc_kernel_event_validate(&events));
    wire_roundtrip(&calls, &events, &decoded_calls, &decoded_events);
    dc_call_reset();
    dc_kernel_event_reset();
    assert(dc_call_restore(&decoded_calls));
    assert(dc_kernel_event_restore(&decoded_events));
    assert(dc_background_pending(&cpu, RETURN) == 0);
    assert(dc_call_validate(&decoded_calls));
    assert(dc_kernel_event_validate(&decoded_events));

    reset_fixture(&cpu);
    arm(&cpu, BG_A);
    set_pending(BG_A, 1);
    assert(trigger(&cpu, BG_A));
    seed_event(&cpu);
    dc_call_capture(&calls);
    dc_kernel_event_capture(&events);
    assert(dc_call_validate(&calls));
    assert(dc_kernel_event_validate(&events));
    wire_roundtrip(&calls, &events, &decoded_calls, &decoded_events);
    for (unsigned malformed = 0; malformed < 4; ++malformed)
        assert_wire_write_rejects(&calls, &events, malformed);
    dc_call_reset();
    dc_kernel_event_reset();
    assert(dc_call_restore(&decoded_calls));
    assert(dc_kernel_event_restore(&decoded_events));
    assert(dc_background_pending(&cpu, RETURN));
    assert(dc_call_validate(&decoded_calls));
    assert(dc_kernel_event_validate(&decoded_events));
}

static void test_precision_roundtrip(void)
{
    CPUState cpu;
    DcGuestRegisters before, after;
    reset_fixture(&cpu);
    seed_register_precision(6, 0x00280018u, 0x2200u, 0x3200u,
                            30 * 65536 + 8192, 40 * 65536 + 16384, 54);
    dc_guest_registers_capture(&cpu, &before);
    arm(&cpu, BG_A);
    set_pending(BG_A, 1);
    assert(trigger(&cpu, BG_A));
    cpu.gpr[6] = 0x00480030u;
    seed_register_precision(6, cpu.gpr[6], 0x2600u, 0x3600u,
                            80 * 65536 + 16384, 90 * 65536 + 24576, 78);
    cpu.hi = 0xAAAAAAAAu;
    cpu.lo = 0xBBBBBBBBu;
    dc_background_resume(&cpu, RETURN);
    dc_guest_registers_capture(&cpu, &after);
    assert(after.words[6] == before.words[6]);
    assert(after.words[32] == before.words[32] && after.words[33] == before.words[33]);
    assert(after.generic.reg[6].live == before.generic.reg[6].live);
    assert(after.generic.reg[6].x16 == before.generic.reg[6].x16);
    assert(after.address.reg[6].bits == before.address.reg[6].bits);
    assert(after.address.reg[6].origin == before.address.reg[6].origin);
}

static void test_tcb_ownership_and_duplicate_prevention(void)
{
    CPUState cpu;
    reset_fixture(&cpu);
    arm(&cpu, BG_A);
    set_pending(BG_A, 1);
    assert(trigger(&cpu, BG_A));
    assert(dc_background_pending(&cpu, RETURN));

    cpu.gpr[31] = HANDLER_RA;
    assert(dc_background_enter(&cpu, HANDLER) == 0);
    assert(dc_background_pending(&cpu, RETURN));

    current_tcb = TCB_B;
    cpu.gpr[29] = STACK_B;
    assert(!dc_background_pending(&cpu, RETURN));
    dc_background_resume(&cpu, RETURN);
    assert(cpu.pc == HANDLER);

    current_tcb = TCB_A;
    cpu.gpr[29] = STACK_A;
    dc_background_resume(&cpu, RETURN);
    assert(cpu.pc == RETURN && dc_background_pending(&cpu, RETURN) == 0);
}

static void test_gates_and_boundaries(void)
{
    CPUState cpu;
    const struct {
        int *gate;
        int value;
    } gates[] = {
        {&camera_active, 0}, {&scripted, 1}, {&scene_flag, 1},
    };

    for (unsigned i = 0; i < sizeof gates / sizeof gates[0]; ++i) {
        reset_fixture(&cpu);
        *gates[i].gate = gates[i].value;
        arm(&cpu, BG_A);
        set_pending(BG_A, 1);
        assert(!trigger(&cpu, BG_A));
        assert(!dc_background_pending(&cpu, RETURN));
    }

    reset_fixture(&cpu);
    set_byte(0x800B92C8u, 4);
    arm(&cpu, BG_A);
    set_pending(BG_A, 1);
    assert(!trigger(&cpu, BG_A));
    assert(!dc_background_pending(&cpu, RETURN));

    reset_fixture(&cpu);
    arm(&cpu, BG_A);
    assert(!trigger(&cpu, BG_A));
    set_pending(BG_A, 1);
    assert(!trigger(&cpu, BG_A));

    reset_fixture(&cpu);
    cpu.gpr[4] = 0x801FFF8Du;
    cpu.gpr[31] = HANDLER_RA;
    assert(!dc_background_enter(&cpu, HANDLER));
    arm(&cpu, BG_MAX);
    set_pending(BG_MAX, 1);
    assert(trigger(&cpu, BG_MAX));

    reset_fixture(&cpu);
    arm(&cpu, BG_A);
    set_pending(BG_A, 1);
    cpu.gpr[29] = STACK_B;
    assert(!trigger(&cpu, BG_A));
    cpu.gpr[29] = STACK_A;
    assert(trigger(&cpu, BG_A));
}

static void test_disabled(void)
{
    CPUState cpu;
    reset_fixture(&cpu);
    set_pending(BG_A, 1);
    arm(&cpu, BG_A);
    DcCallState state;
    dc_call_capture(&state);
    assert(background_frame(&state) == NULL);
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "disabled") == 0) {
        test_disabled();
        return 0;
    }
    test_contract_identity_and_arm();
    test_running_restore_and_call_state_roundtrip();
    test_continuation_wire_armed_and_running();
    test_precision_roundtrip();
    test_tcb_ownership_and_duplicate_prevention();
    test_gates_and_boundaries();
    return 0;
}
