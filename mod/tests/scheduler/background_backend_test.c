#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cpu_state.h"
#include "dc_background.h"
#include "dc_kernel.h"
#include "psx_bios_backend.h"

extern int dc_kernel_activate(void);

const PsxBiosBackend *const psx_bios_builtin_registry[] = {0};
const uint32_t psx_bios_builtin_registry_count = 0;
uint64_t g_dispatch_static_hits;
uint64_t g_psx_bail_first, g_psx_bail_resolved, g_psx_bail_flattened, g_psx_bail_anomaly;
uint64_t s_frame_count;
int g_psx_call_bail, g_psx_cps_mode;

enum {
    RAM_SIZE = 0x200000,
    TCB_A = 0xA000E29Cu,
    STACK = 0x801F0000u,
    BACKGROUND = 0x800B79C0u,
    HANDLER = 0x8002731Cu,
    HANDLER_RA = 0x800272B8u,
    RETURN = 0x8008F6F4u,
    RETURN_RA = 0x8002730Cu,
    STOP = 0x80018000u,
};

static uint8_t ram[RAM_SIZE];
static uint32_t current_tcb;
static unsigned handler_calls, service_calls, original_return_calls;
static uint32_t original_gpr[32], original_hi, original_lo;

static size_t ram_offset(uint32_t address)
{
    return (size_t)(address - 0x80000000u);
}

uint8_t *memory_get_ram_ptr(void) { return ram; }
uint32_t psx_sched_current_tcb(CPUState *cpu) { (void)cpu; return current_tcb; }
int dc_nots_active(void) { return 1; }
int dc_scripted_now(void) { return 0; }
int dc_scene_flag_on(void) { return 0; }
void spu_xpage_capture(int page) { (void)page; }
void psx_fatal_halt(const char *reason) { (void)reason; abort(); }

void psx_bios_hle_configure(int calls, int boot)
{
    assert(calls == 0 && boot == 0);
}

void fntrace_record(CPUState *cpu, uint32_t addr)
{
    assert(cpu && addr);
}

void psx_check_interrupts(CPUState *cpu) { assert(cpu); }
void psx_check_interrupts_at(CPUState *cpu, uint32_t pc) { assert(cpu && pc == STOP); }
void psx_rfe_escape_check(CPUState *cpu) { assert(cpu); }

int hle_bios_dispatch(CPUState *cpu, uint32_t addr, uint32_t phys)
{
    (void)cpu; (void)addr; (void)phys;
    return 0;
}

int dirty_ram_dispatch(CPUState *cpu, uint32_t addr, uint32_t stop_addr)
{
    (void)cpu; (void)addr; (void)stop_addr;
    return 0;
}

void psx_unknown_dispatch(CPUState *cpu, uint32_t addr, uint32_t phys)
{
    (void)cpu; (void)addr; (void)phys; abort();
}

int dc_creature_blood_resume(CPUState *cpu, uint32_t pc)
{
    (void)cpu; (void)pc;
    return 0;
}

int dc_creature_blood_pending(CPUState *cpu, uint32_t pc)
{
    (void)cpu; (void)pc;
    return 0;
}

int dc_kernel_event_resume(CPUState *cpu, uint32_t pc)
{
    (void)cpu; (void)pc;
    return 0;
}

int dc_kernel_event_pending(CPUState *cpu, uint32_t pc)
{
    (void)cpu; (void)pc;
    return 0;
}

void dc_kernel_event_result(int hit) { (void)hit; abort(); }
void dc_kernel_card_arm(uint32_t delay, uint32_t spec) { (void)delay; (void)spec; abort(); }

int psx_dispatch_game_compiled(CPUState *cpu, uint32_t addr)
{
    if (addr == HANDLER) {
        ++handler_calls;
        if (cpu->gpr[31] == HANDLER_RA) {
            assert(cpu->gpr[4] == BACKGROUND);
            cpu->gpr[5] = BACKGROUND;
            ram[ram_offset(BACKGROUND) + 114u] = 1;
            cpu->gpr[31] = RETURN_RA;
            memcpy(original_gpr, cpu->gpr, sizeof original_gpr);
            original_hi = cpu->hi;
            original_lo = cpu->lo;
            cpu->pc = RETURN;
        } else {
            assert(cpu->gpr[31] == RETURN);
            assert(cpu->gpr[4] == BACKGROUND && cpu->gpr[5] == BACKGROUND);
            ++service_calls;
            ram[ram_offset(BACKGROUND) + 114u] &= (uint8_t)~1u;
            ram[ram_offset(BACKGROUND) + 115u] = 6;
            cpu->gpr[4] = 0xDEAD0004u;
            cpu->gpr[6] = 0xDEAD0006u;
            cpu->gpr[31] = RETURN;
            cpu->hi = 0xDEAD1111u;
            cpu->lo = 0xDEAD2222u;
            cpu->pc = 0;
        }
        return 1;
    }
    if (addr == RETURN) {
        ++original_return_calls;
        assert(service_calls == 1 && handler_calls == 2);
        assert(memcmp(cpu->gpr, original_gpr, sizeof original_gpr) == 0);
        assert(cpu->hi == original_hi && cpu->lo == original_lo);
        assert(ram[ram_offset(BACKGROUND) + 114u] == 0);
        assert(ram[ram_offset(BACKGROUND) + 115u] == 6);
        cpu->gpr[31] = STOP;
        cpu->pc = STOP;
        return 1;
    }
    abort();
}

int main(void)
{
    CPUState cpu = {0};
    memset(ram, 0, sizeof ram);
    current_tcb = TCB_A;
    cpu.gpr[29] = STACK;
    cpu.gpr[31] = HANDLER_RA;
    cpu.gpr[4] = BACKGROUND;
    cpu.gpr[6] = 0x60606060u;
    cpu.hi = 0x12345678u;
    cpu.lo = 0x9ABCDEF0u;
    for (unsigned i = 1; i < 32; ++i)
        if (i != 4 && i != 6 && i != 29 && i != 31)
            cpu.gpr[i] = 0x11000000u + i * 0x01010101u;
    assert(dc_kernel_activate());
    psx_dispatch_call(&cpu, HANDLER, STOP);
    assert(cpu.pc == 0 && g_psx_dispatch_depth == 0 && !g_psx_call_bail);
    assert(handler_calls == 2 && service_calls == 1 && original_return_calls == 1);
    return 0;
}
