#include <assert.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dc_pgxp.h"
#include "dc_kernel.h"
#include "dc_kernel_events.h"
#include "dc_register_precision.h"
#include "pgxp.h"
#include "pgxp_cells.h"
#include "pgxp_hooks.h"
#include "test_process.h"

enum {
    RAM_SIZE = 0x200000,
    EVENT_COUNT = HLE_EVCB_SIZE / 0x1C,
    TCB_A = 0xA000E29Cu,
    TCB_B = 0xA000E35Cu,
    STACK = 0x801F0000u,
    RETURN_PC = 0x8001A000u,
    CALLBACK_A = 0x80018000u,
    CALLBACK_B = 0x80018010u,
    CALLBACK_C = 0x80018020u,
    CALLBACK_BAD = 0x80018002u,
    CARD_CALLBACK_HW = 0x80018100u,
    CARD_CALLBACK_SW = 0x80018110u,
    CLASS_A = 0xF1000010u,
    CLASS_B = 0xF1000020u,
    CLASS_C = 0xF1000030u,
    CARD_CLASS_HW = 0xF0000011u,
    CARD_CLASS_SW = 0xF4000001u,
    CARD_SPEC_DONE = 0x0004u,
    CARD_ARM_SPEC = 0x8000u,
    CARD_ARM_DELAY = 3u,
    CARD_OLD_RESULT = 0x0BADF00Du,
    CARD_NATIVE_RESULT = 0x5A5Au,
    SPEC_A = 0x0010u,
    SPEC_B = 0x0020u,
    SPEC_C = 0x0030u,
    EVENT_ENABLED = 0x2000u,
    EVENT_DELIVERED = 0x4000u,
};

static uint8_t ram[RAM_SIZE];
static uint32_t current_tcb;
static unsigned result_calls, result_hits;
static unsigned card_arm_calls, card_callback_calls;
static uint32_t card_arm_delay, card_arm_spec;
static jmp_buf fatal_env;
static int fatal_armed;
static const char *fatal_reason;

static size_t ram_offset(uint32_t address)
{
    return (size_t)(address & 0x1FFFFCu);
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

static uint16_t read_half(uint32_t address)
{
    uint16_t value;
    memcpy(&value, &ram[ram_offset(address)], sizeof value);
    return value;
}

static void write_half(uint32_t address, uint16_t value)
{
    memcpy(&ram[ram_offset(address)], &value, sizeof value);
}

static uint8_t read_byte(uint32_t address)
{
    return ram[ram_offset(address)];
}

static void write_byte(uint32_t address, uint8_t value)
{
    ram[ram_offset(address)] = value;
}

uint32_t psx_sched_current_tcb(CPUState *cpu)
{
    (void)cpu;
    return current_tcb;
}

void psx_fatal_halt(const char *reason)
{
    fatal_reason = reason;
    if (fatal_armed) longjmp(fatal_env, 1);
    fprintf(stderr, "fatal: %s\n", reason);
    abort();
}

void dc_kernel_event_result(int hit)
{
    ++result_calls;
    result_hits += hit != 0;
}

void dc_kernel_card_arm(uint32_t delay, uint32_t spec)
{
    ++card_arm_calls;
    card_arm_delay = delay;
    card_arm_spec = spec;
}

static uint32_t event_address(unsigned index)
{
    assert(index < EVENT_COUNT);
    return 0x80000000u + HLE_EVCB_ADDR + index * 0x1Cu;
}

static void set_event(unsigned index, uint32_t cls, uint32_t status,
                      uint32_t spec, uint32_t mode, uint32_t function)
{
    uint32_t address = event_address(index);
    write_word(address + 0, cls);
    write_word(address + 4, status);
    write_word(address + 8, spec);
    write_word(address + 12, mode);
    write_word(address + 16, function);
}

static void cpu_init(CPUState *cpu)
{
    memset(cpu, 0, sizeof *cpu);
    cpu->read_word = read_word;
    cpu->write_word = write_word;
    cpu->read_half = read_half;
    cpu->write_half = write_half;
    cpu->read_byte = read_byte;
    cpu->write_byte = write_byte;
    cpu->gpr[4] = 0xA0A0A0A0u;
    cpu->gpr[5] = 0xB0B0B0B0u;
    cpu->gpr[6] = 0xC0C0C0C0u;
    cpu->gpr[7] = 0xD0D0D0D0u;
    cpu->gpr[16] = 0x16161616u;
    cpu->gpr[29] = STACK;
    cpu->gpr[31] = RETURN_PC;
    cpu->hi = 0x11112222u;
    cpu->lo = 0x33334444u;
}

static void fixture_reset(CPUState *cpu)
{
    memset(ram, 0, sizeof ram);
    dc_kernel_event_reset();
    current_tcb = TCB_A;
    result_calls = 0;
    result_hits = 0;
    card_arm_calls = 0;
    card_callback_calls = 0;
    card_arm_delay = 0;
    card_arm_spec = 0;
    fatal_reason = NULL;
    fatal_armed = 0;
    cpu_init(cpu);
}

static void guest_return(CPUState *cpu, uint32_t callback)
{
    assert(cpu->pc == callback);
    if (callback == CARD_CALLBACK_HW) {
        assert(cpu->gpr[4] == CARD_CLASS_HW);
        assert(cpu->gpr[5] == CARD_SPEC_DONE);
        assert(cpu->gpr[2] == CARD_OLD_RESULT);
        assert(card_callback_calls == 0);
    } else if (callback == CARD_CALLBACK_SW) {
        assert(cpu->gpr[4] == CARD_CLASS_SW);
        assert(cpu->gpr[5] == CARD_SPEC_DONE);
        assert(cpu->gpr[2] == CARD_OLD_RESULT);
        assert(card_callback_calls == 1);
    }
    if (callback == CARD_CALLBACK_HW || callback == CARD_CALLBACK_SW)
        ++card_callback_calls;
    cpu->pc = RETURN_PC;
    assert(dc_kernel_event_resume(cpu, RETURN_PC));
    if (callback == CARD_CALLBACK_HW) {
        assert(card_arm_calls == 0);
        assert(cpu->gpr[2] == CARD_OLD_RESULT);
    } else if (callback == CARD_CALLBACK_SW) {
        assert(card_arm_calls == 1);
        assert(card_arm_delay == CARD_ARM_DELAY);
        assert(card_arm_spec == CARD_ARM_SPEC);
        assert(cpu->gpr[2] == CARD_NATIVE_RESULT);
    }
}

static uint32_t memory_op(unsigned opcode, unsigned reg)
{
    return (opcode << 26) | (reg << 16);
}

static void seed_register_precision(unsigned reg, uint32_t raw,
                                    uint32_t origin, uint32_t source,
                                    int32_t x16, int32_t y16, uint16_t depth)
{
    dc_pgxp_cop2(memory_op(0x32u, 0u), raw, origin);
    dc_pgxp_project(0, x16, y16, depth, raw, 1);
    pgxp_gte_push_sxy(x16, y16, depth, raw);
    psx_pgxp_cop2(NULL, memory_op(0x3Au, 14u), raw, source);
    psx_pgxp_load(NULL, memory_op(0x23u, reg), source, raw);
}

static void assert_generic_shadow_equal(const PsxRegisterShadow *a,
                                        const PsxRegisterShadow *b)
{
    assert(a->value == b->value);
    assert(a->live == b->live && a->scalar_live == b->scalar_live);
    assert(a->x16 == b->x16 && a->y16 == b->y16 && a->z == b->z);
    assert(a->flags == b->flags);
    assert(a->projection[0] == b->projection[0]);
    assert(a->projection[1] == b->projection[1]);
    assert(a->projection[2] == b->projection[2]);
    assert(a->projection[3] == b->projection[3]);
    assert(a->scalar == b->scalar);
}

static void assert_address_shadow_equal(const PxCell *a, const PxCell *b)
{
    assert(a->raw == b->raw && a->bits == b->bits && a->origin == b->origin);
    assert(a->x == b->x && a->y == b->y && a->z == b->z);
}

static unsigned active_frames(const DcKernelEventState *state)
{
    unsigned count = 0;
    for (unsigned i = 0; i < DC_KERNEL_EVENT_CAPACITY; ++i)
        count += state->frames[i].owner != 0;
    return count;
}

static void test_event_scan_and_register_boundary(void)
{
    CPUState cpu;
    fixture_reset(&cpu);

    set_event(0, CLASS_A, EVENT_ENABLED, SPEC_A, 0x1000u, CALLBACK_A);
    set_event(1, CLASS_A, EVENT_ENABLED, SPEC_A, 0x3000u, CALLBACK_BAD);
    set_event(2, CLASS_A, EVENT_ENABLED, SPEC_A, 0x1000u, 0);
    set_event(3, CLASS_A, EVENT_ENABLED, SPEC_A, 0x2000u, 0);
    set_event(4, CLASS_B, EVENT_ENABLED, SPEC_A, 0x1000u, CALLBACK_B);

    dc_kernel_event_begin(&cpu, CLASS_A, SPEC_A);
    assert(cpu.pc == CALLBACK_A);
    assert(cpu.gpr[4] == CLASS_A && cpu.gpr[5] == SPEC_A);
    assert(cpu.gpr[31] == RETURN_PC);
    assert(result_calls == 0);

    /* The callback changes a later slot before the resumable scan reaches it. */
    write_word(event_address(4) + 0, CLASS_A);
    cpu.gpr[6] = 0x66666666u;
    cpu.gpr[16] = 0xAAAAAAAAu;
    cpu.hi = 0x55556666u;
    cpu.lo = 0x77778888u;
    guest_return(&cpu, CALLBACK_A);

    assert(cpu.pc == CALLBACK_B);
    assert(read_word(event_address(3) + 4) == EVENT_DELIVERED);
    assert(cpu.gpr[4] == CLASS_A && cpu.gpr[5] == SPEC_A);
    assert(cpu.gpr[31] == RETURN_PC);
    assert(cpu.gpr[6] == 0x66666666u);
    assert(cpu.gpr[16] == 0xAAAAAAAAu);
    assert(cpu.hi == 0x55556666u && cpu.lo == 0x77778888u);

    cpu.gpr[6] = 0x99999999u;
    cpu.gpr[16] = 0xBBBBBBBBu;
    cpu.hi = 0x9999AAAAu;
    cpu.lo = 0xBBBBCCCCu;
    guest_return(&cpu, CALLBACK_B);

    assert(cpu.pc == RETURN_PC);
    assert(cpu.gpr[2] == 0);
    assert(cpu.gpr[4] == 0xA0A0A0A0u && cpu.gpr[5] == 0xB0B0B0B0u);
    assert(cpu.gpr[31] == RETURN_PC);
    assert(cpu.gpr[6] == 0x99999999u && cpu.gpr[16] == 0xBBBBBBBBu);
    assert(cpu.hi == 0x9999AAAAu && cpu.lo == 0xBBBBCCCCu);
    assert(read_word(event_address(1) + 4) == EVENT_ENABLED);
    assert(read_word(event_address(2) + 16) == 0);
    assert(read_word(event_address(4) + 4) == EVENT_ENABLED);
    assert(result_calls == 1 && result_hits == 1);
}

static void test_nested_same_parent_identity(void)
{
    CPUState cpu;
    DcKernelEventState state;
    fixture_reset(&cpu);

    set_event(0, CLASS_A, EVENT_ENABLED, SPEC_A, 0x1000u, CALLBACK_A);
    set_event(1, CLASS_B, EVENT_ENABLED, SPEC_B, 0x1000u, CALLBACK_B);

    dc_kernel_event_begin(&cpu, CLASS_A, SPEC_A);
    assert(cpu.pc == CALLBACK_A);
    assert(cpu.gpr[29] == STACK && cpu.gpr[31] == RETURN_PC);

    /* The nested service intentionally keeps the same guest stack and RA. */
    dc_kernel_event_begin(&cpu, CLASS_B, SPEC_B);
    assert(cpu.pc == CALLBACK_B);
    assert(cpu.gpr[4] == CLASS_B && cpu.gpr[5] == SPEC_B);
    assert(dc_kernel_event_pending(&cpu, RETURN_PC));
    dc_kernel_event_capture(&state);
    assert(active_frames(&state) == 2);
    assert(state.frames[0].owner == (TCB_A & 0x1FFFFFFFu));
    assert(state.frames[0].parent == 0);
    assert(state.frames[1].owner == (TCB_A & 0x1FFFFFFFu));
    assert(state.frames[1].parent == 1);
    assert(state.frames[0].stack == STACK && state.frames[1].stack == STACK);
    assert(state.frames[0].continuation == RETURN_PC &&
           state.frames[1].continuation == RETURN_PC);

    cpu.gpr[6] = 0x12345678u;
    cpu.hi = 0xABCDEF01u;
    cpu.lo = 0x10203040u;
    guest_return(&cpu, CALLBACK_B);
    assert(result_calls == 1 && result_hits == 1);
    assert(cpu.gpr[4] == CLASS_A && cpu.gpr[5] == SPEC_A);
    dc_kernel_event_capture(&state);
    assert(active_frames(&state) == 1);
    assert(state.frames[0].parent == 0 && state.frames[0].waiting == 1);

    assert(cpu.pc == RETURN_PC);
    assert(dc_kernel_event_pending(&cpu, RETURN_PC));
    assert(dc_kernel_event_resume(&cpu, RETURN_PC));
    assert(cpu.pc == RETURN_PC);
    assert(cpu.gpr[2] == 0);
    assert(cpu.gpr[4] == 0xA0A0A0A0u && cpu.gpr[5] == 0xB0B0B0B0u);
    assert(cpu.gpr[31] == RETURN_PC);
    assert(cpu.gpr[6] == 0x12345678u);
    assert(cpu.hi == 0xABCDEF01u && cpu.lo == 0x10203040u);
    assert(result_calls == 2 && result_hits == 2);
    dc_kernel_event_capture(&state);
    assert(active_frames(&state) == 0);
}

static void test_precision_subset_restore(void)
{
    CPUState cpu;
    DcGuestRegisters original, callback_state, after;
    const uint32_t original_a0 = 0x0014000Au;
    const uint32_t original_gpr6 = 0x00280018u;
    const uint32_t callback_a0 = 0x003C0028u;
    const uint32_t callback_gpr6 = 0x00480030u;

    fixture_reset(&cpu);
    pgxp_invalidate_all();
    pgxp_addr_reset();
    seed_register_precision(4, original_a0, 0x2000u, 0x3000u,
                            10 * 65536 + 16384, 20 * 65536 + 32768, 42);
    seed_register_precision(6, original_gpr6, 0x2200u, 0x3200u,
                            30 * 65536 + 8192, 40 * 65536 + 16384, 54);
    cpu.gpr[4] = original_a0;
    cpu.gpr[6] = original_gpr6;
    dc_guest_registers_capture(&cpu, &original);
    assert(original.generic.reg[4].live == 1);
    assert(original.generic.reg[6].live == 1);
    assert(original.address.reg[4].bits != 0);
    assert(original.address.reg[6].bits != 0);

    set_event(0, CLASS_A, EVENT_ENABLED, SPEC_A, 0x1000u, CALLBACK_A);
    dc_kernel_event_begin(&cpu, CLASS_A, SPEC_A);
    assert(cpu.pc == CALLBACK_A);

    /* The callback gives $a0 a different shadow, while an unrelated GPR keeps
     * the callback's new word and precision through the masked restore. */
    cpu.gpr[4] = callback_a0;
    seed_register_precision(4, callback_a0, 0x2400u, 0x3400u,
                            60 * 65536 + 4096, 70 * 65536 + 8192, 66);
    cpu.gpr[6] = callback_gpr6;
    seed_register_precision(6, callback_gpr6, 0x2600u, 0x3600u,
                            80 * 65536 + 16384, 90 * 65536 + 24576, 78);
    dc_guest_registers_capture(&cpu, &callback_state);
    assert(callback_state.generic.reg[4].live == 1);
    assert(callback_state.generic.reg[6].live == 1);
    assert(callback_state.address.reg[4].bits != 0);
    assert(callback_state.address.reg[6].bits != 0);
    guest_return(&cpu, CALLBACK_A);

    assert(cpu.pc == RETURN_PC);
    assert(cpu.gpr[2] == 0);
    assert(cpu.gpr[4] == original_a0);
    assert(cpu.gpr[6] == callback_gpr6);
    dc_guest_registers_capture(&cpu, &after);
    assert_generic_shadow_equal(&after.generic.reg[4], &original.generic.reg[4]);
    assert_address_shadow_equal(&after.address.reg[4], &original.address.reg[4]);
    assert_generic_shadow_equal(&after.generic.reg[6], &callback_state.generic.reg[6]);
    assert_address_shadow_equal(&after.address.reg[6], &callback_state.address.reg[6]);
}

enum InvalidCardState {
    INVALID_CARD_KIND,
    INVALID_CARD_CLASS,
    INVALID_CARD_DELAY,
};

static void assert_invalid_card_restore(const DcKernelEventState *live,
                                        enum InvalidCardState invalid)
{
    DcKernelEventState candidate = *live;
    DcKernelEventState after;
    switch (invalid) {
    case INVALID_CARD_KIND:
        candidate.frames[0].kind = 2;
        break;
    case INVALID_CARD_CLASS:
        candidate.frames[0].cls = CARD_CLASS_SW;
        break;
    case INVALID_CARD_DELAY:
        candidate.frames[0].arm_delay = 0x80000000u;
        break;
    }
    assert(!dc_kernel_event_restore(&candidate));
    dc_kernel_event_capture(&after);
    assert(memcmp(&after, live, sizeof after) == 0);
}

static void test_card_completion_order_and_state(void)
{
    CPUState cpu;
    DcKernelEventState deferred, waiting, restored;
    const uint32_t old_result = CARD_OLD_RESULT;

    fixture_reset(&cpu);
    set_event(0, CARD_CLASS_HW, EVENT_ENABLED, CARD_SPEC_DONE, 0x1000u,
              CARD_CALLBACK_HW);
    set_event(1, CARD_CLASS_SW, EVENT_ENABLED, CARD_SPEC_DONE, 0x1000u,
              CARD_CALLBACK_SW);
    cpu.gpr[2] = old_result;
    dc_kernel_event_defer_card(&cpu, CARD_SPEC_DONE, CARD_ARM_DELAY,
                               CARD_ARM_SPEC);
    dc_kernel_event_capture(&deferred);
    assert(active_frames(&deferred) == 1);
    assert(deferred.frames[0].kind == 1);
    assert(deferred.frames[0].class_index == 0);
    assert(deferred.frames[0].cls == CARD_CLASS_HW);
    assert(deferred.frames[0].spec == CARD_SPEC_DONE);
    assert(deferred.frames[0].waiting == 0);
    assert(deferred.frames[0].arm_delay == CARD_ARM_DELAY);
    assert(deferred.frames[0].arm_spec == CARD_ARM_SPEC);

    cpu.gpr[2] = CARD_NATIVE_RESULT;
    assert(dc_kernel_event_complete_native(&cpu));
    assert(cpu.pc == CARD_CALLBACK_HW);
    assert(cpu.gpr[2] == old_result);
    dc_kernel_event_capture(&waiting);
    assert(active_frames(&waiting) == 1);
    assert(waiting.frames[0].kind == 1);
    assert(waiting.frames[0].class_index == 0);
    assert(waiting.frames[0].cls == CARD_CLASS_HW);
    assert(waiting.frames[0].waiting == 1);
    assert(waiting.frames[0].result == CARD_NATIVE_RESULT);

    assert_invalid_card_restore(&waiting, INVALID_CARD_KIND);
    assert_invalid_card_restore(&waiting, INVALID_CARD_CLASS);
    assert_invalid_card_restore(&waiting, INVALID_CARD_DELAY);
    assert(card_arm_calls == 0);

    dc_kernel_event_reset();
    assert(dc_kernel_event_restore(&waiting));
    dc_kernel_event_capture(&restored);
    assert(memcmp(&restored, &waiting, sizeof restored) == 0);
    assert(dc_kernel_event_pending(&cpu, RETURN_PC));

    guest_return(&cpu, CARD_CALLBACK_HW);
    assert(cpu.pc == CARD_CALLBACK_SW);
    assert(card_callback_calls == 1);
    assert(card_arm_calls == 0);
    assert(cpu.gpr[2] == old_result);

    guest_return(&cpu, CARD_CALLBACK_SW);
    assert(cpu.pc == RETURN_PC);
    assert(cpu.gpr[2] == CARD_NATIVE_RESULT);
    assert(card_callback_calls == 2);
    assert(card_arm_calls == 1);
    assert(card_arm_delay == CARD_ARM_DELAY);
    assert(card_arm_spec == CARD_ARM_SPEC);
    assert(result_calls == 2 && result_hits == 2);

    dc_kernel_event_capture(&restored);
    assert(active_frames(&restored) == 0);
}

static void begin_three_waiting(CPUState *cpu)
{
    set_event(0, CLASS_A, EVENT_ENABLED, SPEC_A, 0x1000u, CALLBACK_A);
    set_event(1, CLASS_B, EVENT_ENABLED, SPEC_B, 0x1000u, CALLBACK_B);
    set_event(2, CLASS_C, EVENT_ENABLED, SPEC_C, 0x1000u, CALLBACK_C);
    dc_kernel_event_begin(cpu, CLASS_A, SPEC_A);
    assert(cpu->pc == CALLBACK_A);
    dc_kernel_event_begin(cpu, CLASS_B, SPEC_B);
    assert(cpu->pc == CALLBACK_B);
    dc_kernel_event_begin(cpu, CLASS_C, SPEC_C);
    assert(cpu->pc == CALLBACK_C);
}

enum InvalidState {
    INVALID_VERSION,
    INVALID_PARENT_CYCLE,
    INVALID_ORPHAN,
    INVALID_TWO_CHILDREN,
    INVALID_INDEX,
    INVALID_STACK,
    INVALID_WAITING,
    INVALID_HIT,
};

static void assert_invalid_restore(CPUState *cpu, const DcKernelEventState *live,
                                    enum InvalidState invalid)
{
    DcKernelEventState candidate = *live;
    DcKernelEventState after;
    switch (invalid) {
    case INVALID_VERSION:
        candidate.version++;
        break;
    case INVALID_PARENT_CYCLE:
        candidate.frames[0].parent = 2;
        break;
    case INVALID_ORPHAN:
        candidate.frames[2].parent = DC_KERNEL_EVENT_CAPACITY;
        break;
    case INVALID_TWO_CHILDREN:
        candidate.frames[2].parent = 1;
        break;
    case INVALID_INDEX:
        candidate.frames[1].index = EVENT_COUNT + 1;
        break;
    case INVALID_STACK:
        candidate.frames[0].stack = 0;
        break;
    case INVALID_WAITING:
        candidate.frames[1].waiting = 0;
        break;
    case INVALID_HIT:
        candidate.frames[1].hit = 0;
        break;
    }
    assert(!dc_kernel_event_restore(&candidate));
    dc_kernel_event_capture(&after);
    assert(memcmp(&after, live, sizeof after) == 0);
    assert(cpu->pc == CALLBACK_C);
}

static void test_capture_restore_validation(void)
{
    CPUState cpu;
    DcKernelEventState live, empty;
    fixture_reset(&cpu);
    begin_three_waiting(&cpu);
    dc_kernel_event_capture(&live);
    assert(active_frames(&live) == 3);

    dc_kernel_event_reset();
    dc_kernel_event_capture(&empty);
    assert(empty.version == DC_KERNEL_EVENT_VERSION);
    assert(active_frames(&empty) == 0);
    assert(dc_kernel_event_restore(&live));
    dc_kernel_event_capture(&empty);
    assert(memcmp(&empty, &live, sizeof empty) == 0);

    assert_invalid_restore(&cpu, &live, INVALID_VERSION);
    assert_invalid_restore(&cpu, &live, INVALID_PARENT_CYCLE);
    assert_invalid_restore(&cpu, &live, INVALID_ORPHAN);
    assert_invalid_restore(&cpu, &live, INVALID_TWO_CHILDREN);
    assert_invalid_restore(&cpu, &live, INVALID_INDEX);
    assert_invalid_restore(&cpu, &live, INVALID_STACK);
    assert_invalid_restore(&cpu, &live, INVALID_WAITING);
    assert_invalid_restore(&cpu, &live, INVALID_HIT);
}

static void test_discard_only_owner(void)
{
    CPUState cpu;
    DcKernelEventState state;
    fixture_reset(&cpu);
    set_event(0, CLASS_A, EVENT_ENABLED, SPEC_A, 0x1000u, CALLBACK_A);
    set_event(1, CLASS_B, EVENT_ENABLED, SPEC_B, 0x1000u, CALLBACK_B);

    current_tcb = TCB_A;
    dc_kernel_event_begin(&cpu, CLASS_A, SPEC_A);
    assert(cpu.pc == CALLBACK_A);
    current_tcb = TCB_B;
    dc_kernel_event_begin(&cpu, CLASS_B, SPEC_B);
    assert(cpu.pc == CALLBACK_B);
    dc_kernel_event_capture(&state);
    assert(active_frames(&state) == 2);

    dc_kernel_event_discard_thread(TCB_A);
    dc_kernel_event_capture(&state);
    assert(active_frames(&state) == 1);
    assert(state.frames[0].owner == 0);
    assert(state.frames[1].owner == (TCB_B & 0x1FFFFFFFu));
    assert(state.frames[1].parent == 0);
    assert(dc_kernel_event_pending(&cpu, RETURN_PC));

    current_tcb = TCB_A;
    assert(!dc_kernel_event_pending(&cpu, RETURN_PC));
    current_tcb = TCB_B;
    dc_kernel_event_discard_thread(TCB_B);
    dc_kernel_event_capture(&state);
    assert(active_frames(&state) == 0);
}

static void test_unaligned_callback_rejected(void)
{
    CPUState cpu;
    fixture_reset(&cpu);
    set_event(0, CLASS_A, EVENT_ENABLED, SPEC_A, 0x1000u, CALLBACK_BAD);

    if (setjmp(fatal_env) == 0) {
        fatal_armed = 1;
        dc_kernel_event_begin(&cpu, CLASS_A, SPEC_A);
        fatal_armed = 0;
        assert(!"unaligned callback was accepted");
    } else {
        fatal_armed = 0;
        assert(fatal_reason != NULL);
        assert(strcmp(fatal_reason, "event: unaligned guest callback") == 0);
    }
    dc_kernel_event_reset();
}

int main(void)
{
    test_process_init();
    pgxp_addr_boot();
    assert(g_pgxp_addr_on);
    pgxp_set_enabled(1);
    pgxp_set_cpu_mode(1);
    assert(pgxp_active());
    test_event_scan_and_register_boundary();
    test_nested_same_parent_identity();
    test_precision_subset_restore();
    test_card_completion_order_and_state();
    test_capture_restore_validation();
    test_discard_only_owner();
    test_unaligned_callback_rejected();
    puts("PASS: kernel event state, card completion, nested delivery, and restore validation");
    free(g_px_cell);
    free(g_px_live);
    return 0;
}
