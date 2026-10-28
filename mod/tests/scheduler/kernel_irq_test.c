#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dc_kernel.h"
#include "dc_kernel_snapshot.h"
#include "dc_call_state.h"
#include "interrupts.h"
#include "psx_scheduler.h"
#include "test_process.h"

extern uint8_t g_test_ram[0x200000];
extern uint64_t psx_cycle_count;
extern uint32_t i_stat, i_mask;
extern uint32_t psx_read_word(uint32_t);
extern void psx_write_word(uint32_t, uint32_t);
extern int psx_get_in_exception(void);
extern int g_psx_dispatch_depth;
extern void psx_dispatch_call(CPUState *cpu, uint32_t pc, uint32_t stop);
extern uint16_t g_test_spu_ctrl;
extern void psx_set_midframe_audio_pump(void (*fn)(void));

enum { PCB = 0xA0000000u + HLE_PCB_ADDR,
       TCB_A = 0xA0000000u + HLE_TCB_ADDR, TCB_B = TCB_A + 0xC0,
       RESUME_A = 0x80011000u, RESUME_B = 0x80012000u,
       VERIFIER = 0x80013000u, WORKER = 0x80014000u, NODE = 0x80015000u,
       VERIFIER_TAIL = 0x80013020u, VERIFIER3 = 0x80013040u,
       WORKER2 = 0x80014020u, WORKER3 = 0x80014040u,
       NODE2 = 0x80015020u, NODE3 = 0x80015040u,
       HOOK = 0x80016000u, HOOK_STATE = 0x80017000u,
       WAIT_RETURN = 0x80011080u, WAIT_SP = 0x8001F000u,
       WAIT_RESUME_PC = 0x800000B0u, EVENT_ENABLED = 0x2000u,
       EVENT_DELIVERED = 0x4000u };
static unsigned deliveries, callbacks;
static unsigned dispatch_count;
static int scenario;
static unsigned verifier_tail_hits, link_mutations, followup_callbacks;

extern void (*g_psx_dead_loop_fn)(uint32_t);
static unsigned dead_loop_hits;
static uint32_t dead_loop_pc;
static void note_dead_loop(uint32_t pc)
{
    ++dead_loop_hits;
    dead_loop_pc = pc;
}

static void test_dead_loop_resume_pc(void)
{
    const uint32_t pc = 0x80018000u;
    CPUState cpu = {0};
    interrupts_init();
    i_stat = i_mask = 0;
    dead_loop_hits = 0;
    g_psx_dead_loop_fn = note_dead_loop;
    psx_write_word(pc, 0);
    psx_write_word(pc + 4u, 0);
    for (unsigned i = 0; i < 2050; ++i) psx_check_interrupts_at(&cpu, pc);
    assert(dead_loop_hits == 0 && g_psx_dead_loop_fn);
    interrupts_resync_after_restore();
    psx_write_word(pc, 0x08000000u | ((pc >> 2) & 0x03FFFFFFu));
    for (unsigned i = 0; i < 2050; ++i)
        psx_check_interrupts_at(&cpu, pc + ((i & 1u) ? 8u : 0u));
    assert(dead_loop_hits == 0);
    interrupts_resync_after_restore();
    for (unsigned i = 0; i < 2050; ++i) psx_check_interrupts_at(&cpu, pc);
    assert(dead_loop_hits == 1 && dead_loop_pc == pc && !g_psx_dead_loop_fn);
    for (unsigned i = 0; i < 2050; ++i) psx_check_interrupts_at(&cpu, pc);
    assert(dead_loop_hits == 1);
}

static unsigned animation_returns;
static unsigned sample_service_calls;

static void sample_guest_service(void)
{
    ++sample_service_calls;
}

static void test_spu_sample_deadline_service(void)
{
#ifdef _WIN32
    _putenv_s("PSX_SPU_SAMPLE_EVENTS", "1");
#else
    setenv("PSX_SPU_SAMPLE_EVENTS", "1", 1);
#endif
    interrupts_init();
    sample_service_calls = 0;
    g_test_spu_ctrl = 0x0040u;
    psx_set_midframe_audio_pump(sample_guest_service);

    psx_cycle_count = 0;
    assert(psx_spu_sample_event_cycles_to_next() == 768u);
    psx_cycle_count = 767u;
    assert(psx_spu_sample_event_cycles_to_next() == 1u);
    psx_spu_sample_event_service();
    assert(sample_service_calls == 0u);

    psx_cycle_count = 768u;
    psx_spu_sample_event_service();
    assert(sample_service_calls == 1u);

    g_test_spu_ctrl = 0;
    psx_cycle_count = 1536u;
    assert(psx_spu_sample_event_cycles_to_next() == UINT32_MAX);
    psx_spu_sample_event_service();
    assert(sample_service_calls == 1u);

    puts("PASS: SPU sample deadline bounds and SPUCNT IRQ gating");
}

void pt_anim_frame(CPUState *cpu, uint32_t source, uint32_t destination, uint32_t bones)
{
    (void)cpu;
    assert(source == 0xA004 && destination == 0xA005 && bones == 0xA006);
    ++animation_returns;
}

#ifdef DC_TEST_KERNEL_BACKEND
#include "psx_bios_backend.h"
#include "dc_kernel_events.h"
extern int dc_kernel_activate(void);
const PsxBiosBackend *const psx_bios_builtin_registry[] = {0};
const uint32_t psx_bios_builtin_registry_count;
uint64_t g_dispatch_static_hits;
int g_psx_cps_mode;
void psx_bios_hle_configure(int calls, int boot) { assert(!calls && !boot); }
void fntrace_record(CPUState *cpu, uint32_t pc)
{
    (void)cpu;
    if ((pc & 0x1FFFFFFFu) == HLE_EXC_HANDLER) ++deliveries;
}
int dirty_ram_dispatch(CPUState *cpu, uint32_t pc, uint32_t stop)
{ (void)cpu; (void)pc; (void)stop; assert(!"unexpected interpreter path"); return 0; }
#define psx_dispatch fixture_dispatch

static unsigned counter_event_hits, hardware_event_hits;
static unsigned hardware_followup_hits;
static unsigned event_callback_entries, event_parent_captures;
static unsigned wait_event_returns;
static uint32_t wait_event_desc;
static unsigned irq_chain_seen;
static uint32_t event_parent_a0, event_parent_a1;

enum {
    EVENT_COUNTER_CALLBACK = 0x80018000u,
    EVENT_HARDWARE_CALLBACK = 0x80018020u,
    EVENT_HARDWARE_FOLLOWUP = 0x80018040u,
    EVENT_COUNTER_CLASS = 0xF2000003u,
    EVENT_COUNTER_SPEC = 0x0002u,
    EVENT_HARDWARE_CLASS = 0xF0000001u,
    EVENT_HARDWARE_SPEC = 0x1000u,
    WAIT_EVENT_CLASS = 0xF4000010u,
    WAIT_EVENT_SPEC = 0x0042u,
};

static uint32_t open_enabled_event_mode(CPUState *cpu, uint32_t cls,
                                         uint32_t spec, uint32_t mode,
                                         uint32_t function)
{
    cpu->gpr[9] = 0x08;
    cpu->gpr[4] = cls;
    cpu->gpr[5] = spec;
    cpu->gpr[6] = mode;
    cpu->gpr[7] = function;
    assert(hle_bios_dispatch(cpu, HLE_VEC_B0, HLE_VEC_B0));
    assert((cpu->gpr[2] & 0xFFFF0000u) == 0xF1000000u);
    uint32_t desc = cpu->gpr[2];
    cpu->gpr[9] = 0x0C;
    cpu->gpr[4] = desc;
    assert(hle_bios_dispatch(cpu, HLE_VEC_B0, HLE_VEC_B0));
    assert(cpu->gpr[2] == 1);
    return desc;
}

static uint32_t open_enabled_event(CPUState *cpu, uint32_t cls, uint32_t spec,
                                   uint32_t function)
{
    return open_enabled_event_mode(cpu, cls, spec, 0x1000u, function);
}

static uint32_t event_record_address(uint32_t desc)
{
    return 0x80000000u + HLE_EVCB_ADDR + (desc & 0xFFFFu) * 0x1Cu;
}
#endif
void psx_dispatch(CPUState *cpu, uint32_t pc)
{
    assert(++dispatch_count < 100);
    if (pc == RESUME_A) {
        if (scenario == 6) {
            cpu->gpr[31] = RESUME_A + 0x44;
            dc_anim_call_begin(cpu, RESUME_A + 0x44);
            psx_check_interrupts_at(cpu, RESUME_A + 0x40);
            assert(!"callee poll must switch threads");
        }
#ifdef DC_TEST_KERNEL_BACKEND
        if (scenario == 12) {
            i_stat = i_mask = 0;
            psx_check_interrupts_at(cpu, RESUME_A);
            cpu->gpr[4] = wait_event_desc;
            cpu->gpr[9] = 0x0A;
            cpu->gpr[29] = WAIT_SP;
            cpu->gpr[31] = WAIT_RETURN;
            i_stat = i_mask = 1;
            cpu->pc = HLE_VEC_B0;
            return;
        }
#endif
        if (scenario == 2 || scenario == 3) g_psx_dispatch_depth = 1;
        psx_check_interrupts_at(cpu, RESUME_A);
        if (scenario == 2 || scenario == 3) {
            assert((psx_read_word(PCB) & 0x1FFFFFFF) == (TCB_A & 0x1FFFFFFF));
            assert(cpu->gpr[16] == 0xAAAA);
            if (scenario == 3) {
                cpu->gpr[9] = 0x0F;
                cpu->gpr[4] = 0xFF000001u;
                assert(hle_bios_dispatch(cpu, HLE_VEC_B0, HLE_VEC_B0));
                assert(cpu->gpr[2] == 1);
            }
            g_psx_dispatch_depth = 0;
            psx_cycle_count += 100;
            cpu->pc = RESUME_A + 4;
            psx_check_interrupts_at(cpu, RESUME_A + 4);
        }
        assert(scenario == 0 || scenario == 3 || scenario == 4 ||
               scenario == 7 || scenario == 8 || scenario == 9 ||
               scenario == 10 || scenario == 12);
        psx_request_return_to_lobby();
        assert(!"scheduler exit returned");
    }
    if (scenario == 6 && pc == RESUME_A + 0x40) {
        cpu->gpr[4] = cpu->gpr[5] = cpu->gpr[6] = 0xDEAD;
        cpu->pc = RESUME_A + 0x44;
        psx_dispatch(cpu, cpu->pc);
        assert(!"return continuation escaped scheduler");
    }
    if (scenario == 6 && pc == RESUME_A + 0x44) {
        dc_anim_call_end(cpu, RESUME_A + 0x44);
        dc_anim_call_end(cpu, RESUME_A + 0x44);
        assert(animation_returns == 1);
        psx_request_return_to_lobby();
        assert(!"scheduler exit returned");
    }
    if (pc == RESUME_B) {
        if (scenario == 6) {
            assert(cpu->gpr[16] == 0xBBBB);
            psx_sched_switch_thread(cpu, TCB_A);
            assert(!"thread handoff returned");
        }
#ifdef DC_TEST_KERNEL_BACKEND
        if (scenario == 12) {
            assert(cpu->gpr[16] == 0xBBBB);
            assert(psx_read_word(TCB_A + 136) == WAIT_RESUME_PC);
            assert(psx_read_word(TCB_A + 136) != 0);
            assert(psx_read_word(event_record_address(wait_event_desc) + 4) == EVENT_ENABLED);
            cpu->gpr[9] = 0x07;
            cpu->gpr[4] = WAIT_EVENT_CLASS;
            cpu->gpr[5] = WAIT_EVENT_SPEC;
            cpu->gpr[29] = 0x8001E000u;
            cpu->gpr[31] = RESUME_B + 4;
            assert(hle_bios_dispatch(cpu, HLE_VEC_B0, HLE_VEC_B0));
            assert(psx_read_word(event_record_address(wait_event_desc) + 4) == EVENT_DELIVERED);
            assert(cpu->pc == RESUME_B + 4);
            psx_sched_switch_thread(cpu, TCB_A);
            assert(!"thread handoff returned");
        }
#endif
        assert((scenario == 1 || scenario == 2 || scenario == 5) &&
               cpu->gpr[16] == 0xBBBB);
        cpu->pc = pc;
        psx_request_return_to_lobby();
        assert(!"scheduler exit returned");
    }
#ifdef DC_TEST_KERNEL_BACKEND
    if (scenario == 12 && pc == WAIT_RETURN) {
        assert(cpu->gpr[2] == 1);
        assert(cpu->gpr[4] == wait_event_desc);
        assert(cpu->gpr[29] == WAIT_SP);
        assert(cpu->gpr[31] == WAIT_RETURN);
        assert(psx_read_word(event_record_address(wait_event_desc) + 4) == EVENT_ENABLED);
        assert(wait_event_returns == 0);
        ++wait_event_returns;
        cpu->pc = WAIT_RETURN;
        psx_request_return_to_lobby();
        assert(!"scheduler exit returned");
    }
#endif
    if ((pc & 0x1FFFFFFFu) == HLE_EXC_HANDLER) {
        ++deliveries;
        assert(psx_get_in_exception());
        {
            uint8_t snapshot[1024];
            size_t bytes = dc_kernel_snapshot_bytes();
            assert(bytes <= sizeof snapshot);
            memset(snapshot, 0xA5, sizeof snapshot);
            assert(!dc_kernel_snapshot_write(snapshot, bytes));
            for (size_t i = 0; i < sizeof snapshot; ++i) assert(snapshot[i] == 0xA5);
            memset(snapshot, 0, bytes);
            snapshot[0] = 1; snapshot[4] = 4; snapshot[8] = 8;
            assert(dc_kernel_snapshot_validate(snapshot, bytes));
            assert(!dc_kernel_snapshot_read(snapshot, bytes, cpu));
        }
#ifdef DC_TEST_KERNEL_BACKEND
        if (scenario == 9 || scenario == 10) {
            assert(event_parent_captures == 0);
            ++event_parent_captures;
        }
#endif
        assert(hle_bios_dispatch(cpu, pc, HLE_EXC_HANDLER));
        if (cpu->pc == HOOK) psx_dispatch(cpu, HOOK);
        return;
    }
#ifdef DC_TEST_KERNEL_BACKEND
    if (scenario == 9 && pc == EVENT_COUNTER_CALLBACK) {
        assert(psx_get_in_exception());
        assert(cpu->gpr[4] == EVENT_COUNTER_CLASS);
        assert(cpu->gpr[5] == EVENT_COUNTER_SPEC);
        assert(cpu->gpr[31] == (0x80000000u | HLE_EXC_HANDLER));
        ++counter_event_hits;
        ++event_callback_entries;
        cpu->gpr[6] = 0xC001CAFEu;
        cpu->hi = 0xCAFE;
        cpu->lo = 0xBEEFu;
        uint32_t return_pc = cpu->gpr[31];
        cpu->gpr[31] = 0xDEAD0001u;
        cpu->pc = return_pc;
        return;
    }
    if (scenario == 10 && pc == EVENT_HARDWARE_CALLBACK) {
        assert(psx_get_in_exception());
        assert(cpu->gpr[4] == EVENT_HARDWARE_CLASS);
        assert(cpu->gpr[5] == EVENT_HARDWARE_SPEC);
        assert(cpu->gpr[31] == (0x80000000u | HLE_EXC_HANDLER));
        assert(irq_chain_seen == 1);
        assert(i_stat == 1);
        ++hardware_event_hits;
        ++event_callback_entries;
        cpu->gpr[6] = 0xC001CAFEu;
        cpu->hi = 0xCAFE;
        cpu->lo = 0xBEEFu;
        i_stat = 0;
        uint32_t return_pc = cpu->gpr[31];
        cpu->gpr[31] = 0xDEAD0002u;
        cpu->pc = return_pc;
        return;
    }
    if (scenario == 10 && pc == EVENT_HARDWARE_FOLLOWUP) {
        assert(psx_get_in_exception());
        assert(cpu->gpr[4] == EVENT_HARDWARE_CLASS);
        assert(cpu->gpr[5] == EVENT_HARDWARE_SPEC);
        assert(cpu->gpr[31] == (0x80000000u | HLE_EXC_HANDLER));
        ++hardware_followup_hits;
        ++event_callback_entries;
        uint32_t return_pc = cpu->gpr[31];
        cpu->gpr[31] = 0xDEAD0003u;
        cpu->pc = return_pc;
        return;
    }
#endif
    if (pc == HOOK) {
        assert(scenario == 4 || scenario == 5);
        assert(cpu->gpr[2] == 1 && cpu->gpr[31] == HOOK);
        if (scenario == 5) psx_write_word(PCB, TCB_B);
        cpu->gpr[9] = 0x17;
        assert(hle_bios_dispatch(cpu, HLE_VEC_B0, HLE_VEC_B0));
        assert(!"ReturnFromException returned");
    }
    if (pc == VERIFIER) {
        assert(cpu->gpr[31] == (0x80000000u | HLE_EXC_HANDLER));
#ifdef DC_TEST_KERNEL_BACKEND
        if (scenario == 9) {
            assert(cpu->gpr[4] == event_parent_a0);
            assert(cpu->gpr[5] == event_parent_a1);
            assert(cpu->gpr[6] == 0xC001CAFEu);
        }
#endif
        cpu->gpr[2] = 1;
        if (scenario == 7) {
            cpu->pc = VERIFIER_TAIL;
            return;
        }
        cpu->pc = cpu->gpr[31];
        return;
    }
    if (pc == VERIFIER_TAIL) {
        assert(scenario == 7);
        assert(cpu->gpr[31] == (0x80000000u | HLE_EXC_HANDLER));
        ++verifier_tail_hits;
        cpu->gpr[2] = 1;
        cpu->pc = cpu->gpr[31];
        return;
    }
    if (pc == VERIFIER3) {
        assert(scenario == 8);
        assert(cpu->gpr[31] == (0x80000000u | HLE_EXC_HANDLER));
        cpu->gpr[2] = 1;
        cpu->pc = cpu->gpr[31];
        return;
    }
    if (pc == WORKER2) assert(!"handler ran for a verifier-zero chain node");
    assert(pc == WORKER || (scenario == 8 && pc == WORKER3));
    assert(cpu->gpr[31] == (0x80000000u | HLE_EXC_HANDLER));
    if (scenario == 7 || scenario == 8) assert(cpu->gpr[4] == 1);
    ++callbacks;
    if (pc == WORKER3) ++followup_callbacks;
#ifdef DC_TEST_KERNEL_BACKEND
    if (scenario == 10) {
        irq_chain_seen = 1;
    } else {
        i_stat = 0;
    }
#else
    i_stat = 0;
#endif
    cpu->gpr[16] = 0xDEADBEEFu;
    if (scenario == 8 && pc == WORKER) {
        assert(psx_read_word(NODE) == 0);
        psx_write_word(NODE, NODE2);
        ++link_mutations;
    }
    if ((scenario >= 1 && scenario <= 3) || scenario == 6 || scenario == 12) {
        cpu->gpr[9] = 0x10;
        cpu->gpr[4] = 0xFF000001u;
        assert(hle_bios_dispatch(cpu, HLE_VEC_B0, HLE_VEC_B0));
        assert(!"exception-time thread switch returned");
    }
    cpu->pc = cpu->gpr[31];
}
#ifdef DC_TEST_KERNEL_BACKEND
#undef psx_dispatch
int psx_dispatch_game_compiled(CPUState *cpu, uint32_t pc)
{
    fixture_dispatch(cpu, pc);
    return 1;
}
#else
void psx_dispatch_call(CPUState *cpu, uint32_t pc, uint32_t stop)
{
    assert(stop == (0x80000000u | HLE_EXC_HANDLER));
    psx_dispatch(cpu, pc);
    assert(cpu->pc == stop);
    cpu->pc = 0;
}
#endif

static void run_case(int mode)
{
    int change_thread = mode == 1 || mode == 2 || mode == 5;
    unsigned callbacks_before = callbacks;
    unsigned tail_before = verifier_tail_hits;
    unsigned links_before = link_mutations;
    unsigned followup_before = followup_callbacks;
    unsigned deliveries_before = deliveries;
    CPUState cpu = {0};
    scenario = mode;
    dc_call_reset();
#ifdef DC_TEST_KERNEL_BACKEND
    dc_kernel_event_reset();
#endif
    memset(g_test_ram, 0, sizeof g_test_ram);
    cpu.read_word = psx_read_word;
    cpu.write_word = psx_write_word;
    psx_write_word(0x80, 0x3C1A0000u);
    psx_write_word(0x84, 0x275A0000u | HLE_EXC_HANDLER);
    psx_write_word(0x108, PCB);
    psx_write_word(0x110, TCB_A);
    psx_write_word(0x114, HLE_TCB_SIZE);
    psx_write_word(PCB, TCB_A);
    psx_write_word(TCB_A, 0x4000);
    psx_write_word(TCB_B, 0x4000);
    psx_write_word(TCB_B + 136, RESUME_B);
    psx_write_word(TCB_B + 148, 0x404);
    psx_write_word(TCB_B + 8 + 16 * 4, 0xBBBB);
    psx_write_word(TCB_B + 8 + 29 * 4, 0x8001E000);
    psx_write_word(NODE + 4, WORKER);
    psx_write_word(NODE + 8, VERIFIER);
#ifdef DC_TEST_KERNEL_BACKEND
    if (mode == 8) {
        psx_write_word(NODE2 + 0, NODE3);
        psx_write_word(NODE2 + 4, WORKER2);
        psx_write_word(NODE2 + 8, 0);
        psx_write_word(NODE3 + 4, WORKER3);
        psx_write_word(NODE3 + 8, VERIFIER3);
    }
    if (mode == 9)
        open_enabled_event(&cpu, EVENT_COUNTER_CLASS, EVENT_COUNTER_SPEC,
                           EVENT_COUNTER_CALLBACK);
    if (mode == 10) {
        open_enabled_event(&cpu, EVENT_HARDWARE_CLASS, EVENT_HARDWARE_SPEC,
                           EVENT_HARDWARE_CALLBACK);
        open_enabled_event(&cpu, EVENT_HARDWARE_CLASS, EVENT_HARDWARE_SPEC,
                           EVENT_HARDWARE_FOLLOWUP);
    }
    if (mode == 12)
        wait_event_desc = open_enabled_event_mode(&cpu, WAIT_EVENT_CLASS,
                                                  WAIT_EVENT_SPEC, 0x2000u, 0);
    if (mode == 12) {
        wait_event_returns = 0;
    }
#endif
    cpu.gpr[9] = 3;
    cpu.gpr[4] = 2;
    cpu.gpr[5] = NODE;
    assert(hle_bios_dispatch(&cpu, HLE_VEC_C0, HLE_VEC_C0));
    cpu.gpr[9] = 2;
    assert(hle_bios_dispatch(&cpu, HLE_VEC_C0, HLE_VEC_C0));
    cpu.gpr[9] = (mode == 4 || mode == 5) ? 0x19 : 0x18;
    cpu.gpr[4] = HOOK_STATE;
    psx_write_word(HOOK_STATE, HOOK);
    psx_write_word(HOOK_STATE + 4, 0x8001D000);
    assert(hle_bios_dispatch(&cpu, HLE_VEC_B0, HLE_VEC_B0));
    for (unsigned i = 1; i < 32; ++i) cpu.gpr[i] = 0xA000 + i;
    cpu.gpr[16] = 0xAAAA;
    cpu.gpr[29] = 0x8001F000;
    cpu.hi = 0x1234; cpu.lo = 0x5678;
    cpu.pc = RESUME_A;
    cpu.cop0[12] = 0x401;
    for (unsigned i = 1; i < 32; ++i)
        psx_write_word(TCB_A + 8 + i * 4, cpu.gpr[i]);
    psx_write_word(TCB_A + 136, RESUME_A);
    psx_write_word(TCB_A + 140, cpu.hi);
    psx_write_word(TCB_A + 144, cpu.lo);
    psx_write_word(TCB_A + 148, 0x404);
    psx_cycle_count = 1000;
    interrupts_resync_after_restore();
#ifdef DC_TEST_KERNEL_BACKEND
    event_parent_a0 = cpu.gpr[4];
    event_parent_a1 = cpu.gpr[5];
    event_parent_captures = 0;
    irq_chain_seen = 0;
#endif
    i_stat = i_mask = 1;
    psx_clear_return_to_lobby();
    psx_scheduler_run(&cpu);
    assert(!psx_get_in_exception());
    assert(cpu.gpr[16] == (change_thread ? 0xBBBB : 0xAAAA));
    assert(cpu.pc == (change_thread ? RESUME_B : mode == 3 ? RESUME_A + 4 :
                      mode == 6 ? RESUME_A + 0x44 :
                      mode == 12 ? WAIT_RETURN : RESUME_A));
    assert((psx_read_word(PCB) & 0x1FFFFFFFu) ==
           ((change_thread ? TCB_B : TCB_A) & 0x1FFFFFFFu));
    assert(psx_read_word(TCB_A + 136) ==
           (mode == 2 ? RESUME_A + 4 : mode == 6 ? RESUME_A + 0x40 :
            mode == 12 ? WAIT_RESUME_PC : RESUME_A));
    assert(psx_read_word(TCB_A + 8 + 16 * 4) == 0xAAAA);
    assert(cpu.gpr[29] == (change_thread ? 0x8001E000u : 0x8001F000u));
    assert((cpu.cop0[12] & 0xF) == 1);
    if (!change_thread) assert(cpu.hi == 0x1234 && cpu.lo == 0x5678);
#ifdef DC_TEST_KERNEL_BACKEND
    if (mode == 7) {
        assert(verifier_tail_hits == tail_before + 1);
        assert(callbacks == callbacks_before + 1);
    }
    if (mode == 8) {
        assert(link_mutations == links_before + 1);
        assert(followup_callbacks == followup_before + 1);
        assert(callbacks == callbacks_before + 2);
        assert(psx_read_word(NODE) == NODE2);
    }
    if (mode == 9) {
        assert(counter_event_hits == 1);
        assert(event_callback_entries == 1);
        assert(deliveries == deliveries_before + 1);
    }
    if (mode == 10) {
        assert(hardware_event_hits == 1);
        assert(hardware_followup_hits == 1);
        assert(event_callback_entries == 3);
        assert(deliveries == deliveries_before + 1);
        assert(i_stat == 0);
    }
    if (mode == 12) {
        assert(wait_event_returns == 1);
        assert(deliveries == deliveries_before + 1);
        assert(psx_read_word(TCB_A + 136) == WAIT_RESUME_PC);
        assert(psx_read_word(event_record_address(wait_event_desc) + 4) == EVENT_ENABLED);
        assert(cpu.gpr[2] == 1);
        assert(cpu.gpr[4] == wait_event_desc);
        assert(cpu.gpr[29] == WAIT_SP);
        assert(cpu.gpr[31] == WAIT_RETURN);
    }
#endif
}

int main(void)
{
    test_process_init();
#ifdef DC_TEST_KERNEL_BACKEND
    assert(dc_kernel_activate());
#endif
#ifdef DC_TEST_KERNEL_BACKEND
    const int modes[] = {0, 2, 3, 4, 7, 8, 9, 10, 12};
    for (unsigned i = 0; i < sizeof modes / sizeof modes[0]; ++i) run_case(modes[i]);
    assert(deliveries == 9 && callbacks == 10);
    assert(verifier_tail_hits == 1 && link_mutations == 1 && followup_callbacks == 1);
    assert(counter_event_hits == 1 && hardware_event_hits == 1);
    assert(hardware_followup_hits == 1);
    assert(event_callback_entries == 3);
#else
    for (int mode = 0; mode < 7; ++mode) run_case(mode);
    assert(deliveries == 7 && callbacks == 7);
#endif
    test_dead_loop_resume_pc();
    test_spu_sample_deadline_service();
    puts("PASS: kernel IRQ register restore and exception-time thread handoff");
    return 0;
}
