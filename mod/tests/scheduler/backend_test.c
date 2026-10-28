#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cpu_state.h"
#include "dc_kernel.h"
#include "psx_bios_backend.h"
#include "test_process.h"

extern int dc_kernel_activate(void);
const PsxBiosBackend *const psx_bios_builtin_registry[] = {0};
const uint32_t psx_bios_builtin_registry_count = 0;
uint64_t g_dispatch_static_hits, g_psx_bail_first, g_psx_bail_resolved;
uint64_t g_psx_bail_flattened, g_psx_bail_anomaly;
int g_psx_call_bail, g_psx_cps_mode;
uint64_t s_frame_count;
static unsigned mode, compiled_calls, interrupt_checks;
enum { NORMAL, TAIL, WILD, DIRTY, VECTOR, VECTOR_TRANSFER };
enum { START = 0x80010000, NEXT = 0x80010100, STOP = 0x80010200 };

void psx_bios_hle_configure(int calls, int boot)
{
#ifdef DC_TCB_CPS
    (void)calls; (void)boot; abort();
#else
    assert(calls == 0 && boot == 0);
#endif
}
void fntrace_record(CPUState *cpu, uint32_t pc)
{
    assert(cpu != NULL && pc != 0);
}
void psx_check_interrupts(CPUState *cpu) { assert(cpu != NULL); ++interrupt_checks; }
void psx_check_interrupts_at(CPUState *cpu, uint32_t pc)
{
    assert(cpu != NULL && pc == STOP);
    ++interrupt_checks;
}
void psx_rfe_escape_check(CPUState *cpu) { assert(cpu != NULL); }
int hle_bios_dispatch(CPUState *cpu, uint32_t addr, uint32_t phys)
{
    assert((addr & 0x1FFFFFFF) == phys);
    if (phys != HLE_VEC_B0) return 0;
    cpu->gpr[2] = 123;
    cpu->pc = mode == VECTOR_TRANSFER ? NEXT : 0;
    return 1;
}
int psx_dispatch_game_compiled(CPUState *cpu, uint32_t addr)
{
    if (mode == DIRTY) return 0;
    assert(addr == START || addr == NEXT);
    ++compiled_calls;
    if (mode == TAIL && addr == START) cpu->pc = NEXT;
    else if (mode == WILD && addr == START) { cpu->gpr[31] = NEXT; cpu->pc = 0; }
    else { cpu->gpr[31] = STOP; cpu->pc = STOP; }
    return 1;
}
int dirty_ram_dispatch(CPUState *cpu, uint32_t addr, uint32_t stop)
{
    assert(mode == DIRTY && addr == START && stop == STOP);
    cpu->pc = STOP;
    return 1;
}
void psx_unknown_dispatch(CPUState *cpu, uint32_t addr, uint32_t phys)
{
    (void)cpu; (void)addr; (void)phys; abort();
}

int main(void)
{
    test_process_init();
    assert(dc_kernel_activate());
    assert(psx_bios_active && strcmp(psx_bios_image.image_id, "DC-HLE") == 0);
    assert(g_psx_cps_mode == 1 && !psx_bios_has_selectable());
    for (mode = NORMAL; mode <= VECTOR_TRANSFER; ++mode) {
        CPUState cpu = {0};
        cpu.gpr[29] = 0x801FF000;
        cpu.gpr[31] = STOP;
        compiled_calls = interrupt_checks = 0;
        psx_dispatch_call(&cpu, mode >= VECTOR ? HLE_VEC_B0 : START, STOP);
        assert(cpu.pc == 0 && g_psx_dispatch_depth == 0 && !g_psx_call_bail);
        assert(interrupt_checks == 1);
        assert(compiled_calls == (mode == TAIL || mode == WILD ? 2u :
                                  mode == DIRTY || mode == VECTOR ? 0u : 1u));
        if (mode >= VECTOR) assert(cpu.gpr[2] == 123);
    }
    assert(g_psx_bail_first == 1 && g_psx_bail_flattened == 1);
    return 0;
}

uint32_t psx_sched_current_tcb(CPUState *cpu) { (void)cpu; abort(); }
void dc_kernel_event_result(int hit) { (void)hit; abort(); }
void psx_fatal_halt(const char *reason) { (void)reason; abort(); }
void dc_kernel_card_arm(uint32_t delay, uint32_t spec) { (void)delay; (void)spec; abort(); }
