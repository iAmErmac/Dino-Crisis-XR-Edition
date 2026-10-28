#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu_state.h"
#include "dc_kernel.h"
#include "dc_kernel_events.h"
#include "psx_bios_backend.h"
#include "psx_scheduler.h"
#include "test_process.h"

extern uint8_t g_test_ram[0x200000];
extern uint32_t psx_read_word(uint32_t address);
extern void psx_write_word(uint32_t address, uint32_t value);
extern int dc_kernel_activate(void);
const PsxBiosBackend *const psx_bios_builtin_registry[] = {0};
const uint32_t psx_bios_builtin_registry_count;
uint64_t g_dispatch_static_hits;
int g_psx_cps_mode;
static unsigned callbacks_a, callbacks_b, callbacks_second, returns_a, returns_b;
static uint32_t flag_handle;
static int nested_mode;
enum { PCB = 0xA0000000u + HLE_PCB_ADDR, TCB_A = 0xA0000000u + HLE_TCB_ADDR,
    TCB_B = TCB_A + 0xC0, START = 0x80011000u, RETURN = 0x80011040u,
    EVENT_CALLBACK = 0x80012000u, RESUME = EVENT_CALLBACK + 4, SECOND = 0x80013000u,
    CALLBACK_B = 0x80014000u, CLASS_A = 0xF4000010u, CLASS_B = 0xF4000011u, SPEC = 2 };
void psx_bios_hle_configure(int calls, int boot) { assert(!calls && !boot); }
void fntrace_record(CPUState *cpu, uint32_t pc) { (void)cpu; (void)pc; }
int dirty_ram_dispatch(CPUState *cpu, uint32_t pc, uint32_t stop)
{ (void)cpu; (void)pc; (void)stop; abort(); }
int psx_dispatch_game_compiled(CPUState *cpu, uint32_t pc)
{
    uint32_t owner = psx_sched_current_tcb(cpu) & 0x1FFFFFFFu;
    if (pc == START) {
        cpu->gpr[4] = owner == (TCB_A & 0x1FFFFFFFu) ? CLASS_A : CLASS_B;
        cpu->gpr[5] = SPEC; cpu->gpr[31] = RETURN; cpu->gpr[9] = 7;
        cpu->pc = 0x800000B0u;
    } else if (pc == EVENT_CALLBACK) {
        assert(owner == (TCB_A & 0x1FFFFFFFu));
        assert(cpu->gpr[4] == CLASS_A && cpu->gpr[5] == SPEC && cpu->gpr[31] == RETURN);
        assert(psx_read_word(HLE_EVCB_ADDR + (flag_handle & 0xFFFFu) * 0x1Cu + 4) == 0x4000);
        ++callbacks_a;
        if (nested_mode) {
            cpu->gpr[4] = CLASS_B; cpu->gpr[5] = SPEC; cpu->gpr[9] = 7;
            cpu->pc = 0x800000B0u;
            return 1;
        }
        assert(callbacks_a == 1);
        static DcKernelEventState pending;
        dc_kernel_event_capture(&pending); dc_kernel_event_reset(); assert(dc_kernel_event_restore(&pending));
        cpu->write_word(cpu->gpr[29] - 4, cpu->gpr[31]); cpu->gpr[29] -= 16;
        cpu->gpr[31] = RESUME;
        psx_sched_switch_thread(cpu, TCB_B);
        abort();
    } else if (pc == RESUME) {
        assert(owner == (TCB_A & 0x1FFFFFFFu));
        cpu->gpr[29] += 16; cpu->gpr[31] = cpu->read_word(cpu->gpr[29] - 4);
        cpu->gpr[4] = 0xDEAD; cpu->gpr[5] = 0xBEEF; cpu->gpr[6] = 0x123456;
        cpu->hi = 0x6789; cpu->lo = 0xABCD;
        cpu->pc = cpu->gpr[31];
    } else if (pc == SECOND) {
        assert(cpu->gpr[4] == CLASS_A && cpu->gpr[5] == SPEC && cpu->gpr[31] == RETURN);
        assert(cpu->gpr[6] == 0x123456 && cpu->hi == 0x6789 && cpu->lo == 0xABCD);
        ++callbacks_second; cpu->gpr[4] = 0; cpu->pc = cpu->gpr[31];
    } else if (pc == CALLBACK_B) {
        assert(owner == (TCB_B & 0x1FFFFFFFu) || nested_mode);
        assert(cpu->gpr[4] == CLASS_B && cpu->gpr[5] == SPEC);
        ++callbacks_b; cpu->gpr[4] = 0xCAFE; cpu->pc = cpu->gpr[31];
        if (nested_mode) g_psx_call_bail = 1;
    } else if (pc == RETURN) {
        assert(cpu->gpr[29] == 0x8001F000u && cpu->gpr[31] == RETURN && cpu->gpr[2] == 0);
        assert(cpu->gpr[5] == SPEC);
        if (owner == (TCB_B & 0x1FFFFFFFu)) {
            assert(cpu->gpr[4] == CLASS_B && callbacks_b == 1);
            ++returns_b; cpu->gpr[31] = RETURN + 4;
            psx_sched_switch_thread(cpu, TCB_A); abort();
        }
        assert(cpu->gpr[4] == CLASS_A && callbacks_a == 1 && callbacks_second == 1);
        ++returns_a; cpu->pc = RETURN;
        psx_request_return_to_lobby(); abort();
    } else abort();
    return 1;
}
static uint32_t open_event(CPUState *cpu, uint32_t cls, uint32_t mode, uint32_t callback)
{
    cpu->gpr[9] = 8; cpu->gpr[4] = cls; cpu->gpr[5] = SPEC; cpu->gpr[6] = mode; cpu->gpr[7] = callback;
    assert(hle_bios_dispatch(cpu, HLE_VEC_B0, HLE_VEC_B0));
    uint32_t handle = cpu->gpr[2]; assert(handle != 0xFFFFFFFFu);
    cpu->gpr[9] = 0x0C; cpu->gpr[4] = handle;
    assert(hle_bios_dispatch(cpu, HLE_VEC_B0, HLE_VEC_B0));
    return handle;
}
int main(void)
{
    test_process_init(); assert(dc_kernel_activate());
    CPUState cpu = {0}; cpu.read_word = psx_read_word; cpu.write_word = psx_write_word;
    memset(g_test_ram, 0, sizeof g_test_ram);
    psx_write_word(0x108, PCB); psx_write_word(PCB, TCB_A);
    psx_write_word(0x110, TCB_A); psx_write_word(0x114, HLE_TCB_SIZE);
    for (unsigned i = 0; i < 2; ++i) {
        uint32_t tcb = TCB_A + i * 0xC0;
        psx_write_word(tcb, 0x4000); psx_write_word(tcb + 136, START);
        psx_write_word(tcb + 148, 0x404); psx_write_word(tcb + 8 + 29 * 4, 0x8001F000);
    }
    flag_handle = open_event(&cpu, CLASS_A, 0x2000, 0);
    open_event(&cpu, CLASS_A, 0x1000, EVENT_CALLBACK);
    open_event(&cpu, CLASS_A, 0x1000, SECOND);
    open_event(&cpu, CLASS_B, 0x1000, CALLBACK_B);
    dc_kernel_event_reset(); psx_scheduler_run(&cpu);
    assert(callbacks_a == 1 && callbacks_b == 1 && callbacks_second == 1 && returns_a == 1 && returns_b == 1);
    static DcKernelEventState state; dc_kernel_event_capture(&state);
    for (unsigned i = 0; i < DC_KERNEL_EVENT_CAPACITY; ++i) assert(!state.frames[i].owner);
    cpu.gpr[4] = CLASS_B; cpu.gpr[5] = SPEC; cpu.gpr[31] = RETURN; cpu.gpr[9] = 7;
    nested_mode = 1;
    psx_dispatch_call(&cpu, 0x800000B0u, RETURN);
    assert(cpu.pc == 0 && cpu.gpr[4] == CLASS_B && cpu.gpr[2] == 0);
    assert(callbacks_b == 2 && returns_b == 1);
    psx_write_word(HLE_EVCB_ADDR + (flag_handle & 0xFFFFu) * 0x1Cu + 4, 0x2000);
    cpu.gpr[4] = CLASS_A; cpu.gpr[5] = SPEC; cpu.gpr[31] = RETURN; cpu.gpr[9] = 7;
    psx_dispatch_call(&cpu, 0x800000B0u, RETURN);
    assert(cpu.pc == 0 && cpu.gpr[4] == CLASS_A && cpu.gpr[5] == SPEC && cpu.gpr[31] == RETURN);
    assert(callbacks_a == 2 && callbacks_second == 2 && callbacks_b == 3 && returns_a == 1 && returns_b == 1);
    assert(!g_psx_call_bail && g_psx_bail_resolved >= 2);
    psx_sched_set_current_tcb(&cpu, TCB_B);
    cpu.gpr[4] = CLASS_B; cpu.gpr[5] = SPEC; cpu.gpr[31] = RETURN; cpu.gpr[9] = 7;
    assert(hle_bios_dispatch(&cpu, HLE_VEC_B0, HLE_VEC_B0));
    assert(cpu.pc == CALLBACK_B && dc_kernel_event_pending(&cpu, RETURN));
    psx_sched_set_current_tcb(&cpu, TCB_A);
    cpu.gpr[9] = 0x0F; cpu.gpr[4] = 0xFF000001u; cpu.gpr[31] = RETURN;
    psx_dispatch_call(&cpu, 0x800000B0u, RETURN);
    assert(cpu.gpr[2] == 1 && psx_read_word(TCB_B) == 0x1000);
    dc_kernel_event_capture(&state);
    for (unsigned i = 0; i < DC_KERNEL_EVENT_CAPACITY; ++i) assert(!state.frames[i].owner);
    puts("PASS: real kernel event callbacks survive scheduler handoff and original return interception");
    return 0;
}
