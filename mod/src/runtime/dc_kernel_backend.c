#include "cpu_state.h"
#include "dc_kernel.h"
#include "dc_creature_blood.h"
#include "dc_background.h"
#include "dc_kernel_events.h"
#include "interrupts.h"
#include "psx_bios_backend.h"

#include <stdint.h>

extern int psx_dispatch_game_compiled(CPUState *cpu, uint32_t addr);
extern int dirty_ram_dispatch(CPUState *cpu, uint32_t addr, uint32_t stop_addr);
extern void psx_unknown_dispatch(CPUState *cpu, uint32_t addr, uint32_t phys);
extern void fntrace_record(CPUState *cpu, uint32_t addr);
extern uint64_t g_dispatch_static_hits;
extern int g_psx_cps_mode;

static void check_return(CPUState *cpu, uint32_t stop_addr)
{
    if (stop_addr) {
        psx_check_interrupts_at(cpu, stop_addr);
        if (cpu->pc == stop_addr) cpu->pc = 0;
    } else {
        psx_check_interrupts(cpu);
    }
}

static void dispatch(CPUState *cpu, uint32_t addr, uint32_t stop_addr)
{
    int outermost = g_psx_dispatch_depth++ == 0;
    uint32_t entry_sp = cpu->gpr[29];
    for (;;) {
        dc_background_resume(cpu, addr);
        fntrace_record(cpu, addr);
        cpu->pc = 0;
        uint32_t phys = addr & 0x1FFFFFFFu;
        int found = dc_background_enter(cpu, addr);
        if (!found) found = dc_creature_blood_resume(cpu, addr);
        if (!found) found = dc_kernel_event_resume(cpu, addr);
        if (!found) found = hle_bios_dispatch(cpu, addr, phys);
        if (found && cpu->pc == 0 && phys != HLE_EXC_HANDLER)
            cpu->pc = cpu->gpr[31];
        if (!found) {
            found = psx_dispatch_game_compiled(cpu, addr);
            if (found) ++g_dispatch_static_hits;
        }
        if (!found) found = dirty_ram_dispatch(cpu, addr, stop_addr);
        if (!found) psx_unknown_dispatch(cpu, addr, phys);
        if (!cpu->pc && (dc_creature_blood_pending(cpu, cpu->gpr[31]) ||
                         dc_background_pending(cpu, cpu->gpr[31]))) cpu->pc = cpu->gpr[31];
        psx_rfe_escape_check(cpu);
        if (cpu->pc && (dc_kernel_event_pending(cpu, cpu->pc) ||
                        dc_creature_blood_pending(cpu, cpu->pc) ||
                        dc_background_pending(cpu, cpu->pc))) {
            if (g_psx_call_bail) {
                g_psx_call_bail = 0;
                ++g_psx_bail_resolved;
            }
            addr = cpu->pc;
            continue;
        }

        if (g_psx_call_bail) {
            if (stop_addr && cpu->pc == stop_addr && cpu->gpr[29] == entry_sp) {
                g_psx_call_bail = 0;
                ++g_psx_bail_resolved;
                cpu->pc = 0;
                --g_psx_dispatch_depth;
                if (outermost) check_return(cpu, stop_addr);
                return;
            }
            if (!outermost) {
                --g_psx_dispatch_depth;
                return;
            }
            g_psx_call_bail = 0;
            ++g_psx_bail_flattened;
            addr = cpu->pc;
            continue;
        }

        if (cpu->pc == 0) {
            if (stop_addr && (cpu->gpr[29] != entry_sp || cpu->gpr[31] != stop_addr)) {
                ++g_psx_bail_first;
                cpu->pc = cpu->gpr[31];
                if (outermost) {
                    ++g_psx_bail_flattened;
                    addr = cpu->pc;
                    continue;
                }
                g_psx_call_bail = 1;
            }
            --g_psx_dispatch_depth;
            if (outermost) check_return(cpu, stop_addr);
            return;
        }

        if (stop_addr && cpu->pc == stop_addr) {
            int was_exception_stack = (uint32_t)(entry_sp - 0x1F800000u) < 0x400u;
            int is_exception_stack = (uint32_t)(cpu->gpr[29] - 0x1F800000u) < 0x400u;
            int exception_return = cpu->gpr[31] == stop_addr &&
                                   was_exception_stack != is_exception_stack;
            if (cpu->gpr[29] == entry_sp || exception_return) {
                cpu->pc = 0;
                --g_psx_dispatch_depth;
                if (outermost) check_return(cpu, stop_addr);
                return;
            }
        }
        addr = cpu->pc;
    }
}

static void dispatch_entry(CPUState *cpu, uint32_t addr)
{
    dispatch(cpu, addr, 0);
}

static void dispatch_call(CPUState *cpu, uint32_t addr, uint32_t return_addr)
{
    dispatch(cpu, addr, return_addr);
}

static const PsxBiosImageInfo kernel_image = {
    .image_id = "DC-HLE",
    .image_bundled = 1,
};

static const PsxBiosBackend kernel_backend = {
    .image = &kernel_image,
    .dispatch = dispatch_entry,
    .dispatch_call = dispatch_call,
};

int dc_kernel_activate(void)
{
    if (!psx_bios_register(&kernel_backend)) return 0;
    if (!psx_bios_activate(&kernel_backend)) return 0;
    g_psx_cps_mode = 1;
    return 1;
}
