#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dc_kernel.h"
#include "dc_creature_blood.h"
#include "psx_bios_backend.h"
#include "psx_cycles.h"
#include "psx_scheduler.h"
#include "test_process.h"

extern uint8_t g_test_ram[0x200000];
extern uint64_t psx_cycle_count;
extern uint64_t psx_next_service_cycle;
extern uint32_t i_stat, i_mask;
extern uint32_t psx_read_word(uint32_t);
extern void psx_write_word(uint32_t, uint32_t);
extern int dc_kernel_activate(void);
extern uint32_t hle_bios_boot(CPUState *cpu, const char *exe_path,
                              uint32_t stack_base);

const PsxBiosBackend *const psx_bios_builtin_registry[] = {0};
const uint32_t psx_bios_builtin_registry_count;
uint64_t g_dispatch_static_hits;
int g_psx_cps_mode;

void fntrace_record(CPUState *cpu, uint32_t pc)
{ (void)cpu; (void)pc; abort(); }

int dirty_ram_dispatch(CPUState *cpu, uint32_t pc, uint32_t stop)
{ (void)cpu; (void)pc; (void)stop; abort(); }

int psx_dispatch_game_compiled(CPUState *cpu, uint32_t pc)
{ (void)cpu; (void)pc; abort(); }

int dc_creature_blood_pending(CPUState *cpu, uint32_t pc)
{ (void)cpu; (void)pc; abort(); }

int dc_creature_blood_resume(CPUState *cpu, uint32_t pc)
{ (void)cpu; (void)pc; abort(); }

enum {
    WAIT_EVENT_CLASS = 0xF4000010u,
    WAIT_EVENT_SPEC = 0x0042u,
    EVENT_DISABLED = 0x1000u,
    EVENT_ENABLED = 0x2000u,
    EVENT_DELIVERED = 0x4000u,
    START_CYCLES = 1000u,
    DELIVERY_CYCLES = 1012u,
    WAIT_SP = 0x801F0000u,
    WAIT_RA = 0x80012340u,
};

static uint32_t wait_event_desc;
static uint64_t delivery_cycle;
static unsigned service_calls;
static unsigned delivery_attempts;

static uint8_t read_byte(uint32_t address)
{
    return g_test_ram[address & 0x1FFFFFu];
}

static void write_byte(uint32_t address, uint8_t value)
{
    g_test_ram[address & 0x1FFFFFu] = value;
}

static void write_le32(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8);
    dst[2] = (uint8_t)(value >> 16);
    dst[3] = (uint8_t)(value >> 24);
}

static void make_fixture(const char *path)
{
    uint8_t header[0x800] = {0};
    static const uint8_t body[4] = {0x11, 0x22, 0x33, 0x44};
    FILE *file;

    memcpy(header, "PS-X EXE", 8);
    write_le32(header + 0x10, 0x80012000u);
    write_le32(header + 0x14, 0x80023450u);
    write_le32(header + 0x18, 0x80012000u);
    write_le32(header + 0x1C, (uint32_t)sizeof body);
    file = fopen(path, "wb");
    assert(file != NULL);
    assert(fwrite(header, 1, sizeof header, file) == sizeof header);
    assert(fwrite(body, 1, sizeof body, file) == sizeof body);
    assert(fclose(file) == 0);
}

static uint32_t event_record_address(uint32_t desc)
{
    return 0x80000000u + HLE_EVCB_ADDR + (desc & 0xFFFFu) * 0x1Cu;
}

static uint32_t event_status(void)
{
    return psx_read_word(event_record_address(wait_event_desc) + 4u);
}

static void bios_call(CPUState *cpu, uint32_t function)
{
    cpu->gpr[9] = function;
    cpu->pc = HLE_VEC_B0;
    assert(hle_bios_dispatch(cpu, HLE_VEC_B0, HLE_VEC_B0));
}

static void open_event(CPUState *cpu)
{
    cpu->gpr[4] = WAIT_EVENT_CLASS;
    cpu->gpr[5] = WAIT_EVENT_SPEC;
    cpu->gpr[6] = 0x2000u;
    cpu->gpr[7] = 0u;
    bios_call(cpu, 0x08u);
    assert((cpu->gpr[2] & 0xFFFF0000u) == 0xF1000000u);
    wait_event_desc = cpu->gpr[2];
}

void wait_event_timing_service(void)
{
    if (psx_cycle_count < delivery_cycle) return;
    ++service_calls;
    ++delivery_attempts;
    assert(psx_cycle_count == DELIVERY_CYCLES);
    assert(event_status() == EVENT_ENABLED);
    psx_write_word(event_record_address(wait_event_desc) + 4u,
                   EVENT_DELIVERED);
    psx_next_service_cycle = UINT64_MAX;
}

static void wait_for_event(CPUState *cpu)
{
    cpu->gpr[4] = wait_event_desc;
    cpu->gpr[29] = WAIT_SP;
    cpu->gpr[31] = WAIT_RA;
    cpu->gpr[2] = 0xDEADC0DEu;
    bios_call(cpu, 0x0Au);
}

static void boot_fixture(CPUState *cpu)
{
    static const char fixture_path[] = "dc_wait_event_timing_fixture.psx";

    test_process_init();
    assert(dc_kernel_activate());
    memset(g_test_ram, 0, sizeof g_test_ram);
    memset(cpu, 0, sizeof *cpu);
    cpu->read_word = psx_read_word;
    cpu->write_word = psx_write_word;
    cpu->read_byte = read_byte;
    cpu->write_byte = write_byte;
    make_fixture(fixture_path);
    assert(hle_bios_boot(cpu, fixture_path, 0x801FF000u) == 0x80012000u);
    assert(remove(fixture_path) == 0);
}

static void run_timing_contract(void)
{
    CPUState cpu;
    const uint64_t initial_cycles = START_CYCLES;

    boot_fixture(&cpu);
    open_event(&cpu);
    assert(event_status() == EVENT_DISABLED);

    /* A disabled event returns immediately and does not touch guest time. */
    psx_cycle_count = initial_cycles;
    psx_next_service_cycle = UINT64_MAX;
    service_calls = 0;
    delivery_attempts = 0;
    wait_for_event(&cpu);
    assert(cpu.gpr[2] == 0u);
    assert(psx_cycle_count == initial_cycles);
    assert(service_calls == 0u);
    assert(delivery_attempts == 0u);
    assert(event_status() == EVENT_DISABLED);

    /* A delivered event is consumed immediately and likewise has no charge. */
    psx_write_word(event_record_address(wait_event_desc) + 4u,
                   EVENT_DELIVERED);
    cpu.gpr[2] = 0u;
    wait_for_event(&cpu);
    assert(cpu.gpr[2] == 1u);
    assert(psx_cycle_count == initial_cycles);
    assert(service_calls == 0u);
    assert(event_status() == EVENT_ENABLED);

    /* The enabled wait must reach the controlled device deadline first. */
    cpu.gpr[4] = wait_event_desc;
    bios_call(&cpu, 0x0Cu);
    assert(cpu.gpr[2] == 1u);
    assert(event_status() == EVENT_ENABLED);
    psx_cycle_count = initial_cycles;
    delivery_cycle = DELIVERY_CYCLES;
    psx_next_service_cycle = delivery_cycle;
    service_calls = 0;
    delivery_attempts = 0;
    wait_for_event(&cpu);
    assert(cpu.gpr[2] == 1u);
    assert(psx_cycle_count == DELIVERY_CYCLES);
    assert(service_calls == 1u);
    assert(delivery_attempts == 1u);
    assert(event_status() == EVENT_ENABLED);
    assert(cpu.gpr[4] == wait_event_desc);
    assert(cpu.gpr[29] == WAIT_SP);
    assert(cpu.gpr[31] == WAIT_RA);

    puts("PASS: WaitEvent charges real polling time, services delivery at the deadline, consumes the handle, and preserves GPR/SP/RA");
}

int main(void)
{
    run_timing_contract();
    return 0;
}
