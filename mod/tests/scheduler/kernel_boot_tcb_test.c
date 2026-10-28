#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dc_kernel.h"
#include "dc_creature_blood.h"
#include "psx_bios_backend.h"
#include "psx_scheduler.h"
#include "test_process.h"

extern uint8_t g_test_ram[0x200000];
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
    MAIN_TCB = 0xA0000000u + HLE_TCB_ADDR,
    ENTRY_PC = 0x80012000u,
    ENTRY_GP = 0x80023450u,
    ENTRY_SP = 0x801FF000u,
    TEXT_ADDR = 0x80012000u
};

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
    write_le32(header + 0x10, ENTRY_PC);
    write_le32(header + 0x14, ENTRY_GP);
    write_le32(header + 0x18, TEXT_ADDR);
    write_le32(header + 0x1C, (uint32_t)sizeof body);
    file = fopen(path, "wb");
    assert(file != NULL);
    assert(fwrite(header, 1, sizeof header, file) == sizeof header);
    assert(fwrite(body, 1, sizeof body, file) == sizeof body);
    assert(fclose(file) == 0);
}

static void boot_contract(void)
{
    static const char fixture_path[] = "dc_boot_tcb_contract_fixture.psx";
    CPUState cpu = {0};
    uint32_t restored_pc;

    test_process_init();
    assert(dc_kernel_activate());
    memset(g_test_ram, 0, sizeof g_test_ram);
    cpu.read_word = psx_read_word;
    cpu.write_word = psx_write_word;
    cpu.read_byte = read_byte;
    cpu.write_byte = write_byte;
    make_fixture(fixture_path);

    assert(hle_bios_boot(&cpu, fixture_path, ENTRY_SP) == ENTRY_PC);
    assert(psx_read_word(MAIN_TCB + 8u + 28u * 4u) == ENTRY_GP);
    assert(psx_read_word(MAIN_TCB + 8u + 29u * 4u) == ENTRY_SP);
    assert(psx_read_word(MAIN_TCB + 8u + 30u * 4u) == ENTRY_SP);
    assert(psx_read_word(MAIN_TCB + 136u) == ENTRY_PC);
    assert(psx_read_word(MAIN_TCB + 148u) == 0u);

    cpu.gpr[1] = 0xDEADBEEFu;
    cpu.gpr[28] = 0x11111111u;
    cpu.gpr[29] = 0x22222222u;
    cpu.gpr[30] = 0x33333333u;
    cpu.pc = 0u;
    restored_pc = psx_sched_restore_context(&cpu, MAIN_TCB);
    cpu.pc = restored_pc;
    assert(cpu.pc == ENTRY_PC);
    assert(cpu.gpr[1] == 0u);
    assert(cpu.gpr[28] == ENTRY_GP);
    assert(cpu.gpr[29] == ENTRY_SP);
    assert(cpu.gpr[30] == ENTRY_SP);

    assert(remove(fixture_path) == 0);
    puts("PASS: hle_bios_boot seeds the main TCB and restore_context round-trips it");
}

int main(void)
{
    boot_contract();
    return 0;
}
