#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dc_kernel.h"
#include "dc_kernel_events.h"
#include "psx_bios_backend.h"
#include "psx_scheduler.h"
#include "test_process.h"

extern uint8_t g_test_ram[0x200000];
extern uint32_t psx_read_word(uint32_t);
extern void psx_write_word(uint32_t, uint32_t);
extern int dc_kernel_activate(void);
const PsxBiosBackend *const psx_bios_builtin_registry[] = {0};
const uint32_t psx_bios_builtin_registry_count;
uint64_t g_dispatch_static_hits;
int g_psx_cps_mode;
enum { PCB = 0xA0000000u + HLE_PCB_ADDR, TCB_A = 0xA0000000u + HLE_TCB_ADDR,
    TCB_B = TCB_A + 0xC0, START = 0x80011000u, RETURN = START + 4,
    HW = 0x80012000u, SW = 0x80013000u, RESUME = HW + 4,
    OTHER = 0x80014000u, BUFFER = 0x80016000u, SEED = 0x12345678u };
static unsigned reads, hardware, software, other_runs, returns;
static int delayed, final_call;
static uint8_t read_byte(uint32_t a) { return g_test_ram[a & 0x1FFFFFu]; }
static void write_byte(uint32_t a, uint8_t v) { g_test_ram[a & 0x1FFFFFu] = v; }
int memcard_is_present(int card) { assert(card == 0); return 1; }
int memcard_read_sector(int card, int sector, uint8_t *buf)
{
    assert(card == 0 && sector == 7); ++reads;
    if (reads > (delayed ? 2u : 1u)) return -1;
    for (unsigned i = 0; i < 128; ++i) buf[i] = (uint8_t)(i ^ 0x5A);
    return 0;
}
int memcard_write_sector(int card, int sector, const uint8_t *buf)
{ (void)card; (void)sector; (void)buf; abort(); }
void memcard_flush(int card) { (void)card; abort(); }
int memcard_last_flush_result(int card) { (void)card; abort(); }
void psx_bios_hle_configure(int calls, int boot) { assert(!calls && !boot); }
void fntrace_record(CPUState *cpu, uint32_t pc) { (void)cpu; (void)pc; }
int dirty_ram_dispatch(CPUState *cpu, uint32_t pc, uint32_t stop)
{ (void)cpu; (void)pc; (void)stop; abort(); }
static void read_card(CPUState *cpu)
{
    cpu->gpr[9] = 0x4F; cpu->gpr[4] = 0; cpu->gpr[5] = 7;
    cpu->gpr[6] = BUFFER; cpu->gpr[2] = SEED; cpu->gpr[31] = RETURN;
}
int psx_dispatch_game_compiled(CPUState *cpu, uint32_t pc)
{
    if (pc == START) {
        read_card(cpu); cpu->pc = 0x800000B0u;
    } else if (pc == HW || pc == SW) {
        assert(cpu->gpr[4] == (pc == HW ? 0xF0000011u : 0xF4000001u));
        assert(cpu->gpr[5] == ((!delayed && final_call) ? 0x8000u : 4u));
        assert(cpu->gpr[31] == RETURN && cpu->gpr[29] == 0x8001F000u);
        if (pc == HW) {
            ++hardware; assert(hardware == software + 1 && cpu->gpr[2] == SEED);
            if (!final_call) {
                static DcKernelEventState pending;
                dc_kernel_event_capture(&pending); dc_kernel_event_reset();
                assert(dc_kernel_event_restore(&pending));
                cpu->gpr[31] = RESUME;
                psx_sched_switch_thread(cpu, TCB_B); abort();
            }
        } else {
            ++software; assert(hardware == software && cpu->gpr[2] == 0xAB);
        }
        cpu->gpr[2] = 0xAB; cpu->gpr[4] = 0xDEAD; cpu->pc = cpu->gpr[31];
    } else if (pc == RESUME) {
        cpu->gpr[31] = RETURN; cpu->gpr[2] = 0xAB; cpu->pc = RETURN;
    } else if (pc == OTHER) {
        ++other_runs;
        if (delayed) {
            read_card(cpu);
            assert(hle_bios_dispatch(cpu, HLE_VEC_B0, HLE_VEC_B0));
            assert(cpu->gpr[2] == 0 && cpu->pc == 0);
        }
        cpu->gpr[31] = OTHER;
        psx_sched_switch_thread(cpu, TCB_A); abort();
    } else if (pc == RETURN) {
        ++returns; assert(!final_call && cpu->gpr[2] == 1);
        assert(cpu->gpr[4] == 0 && cpu->gpr[5] == 7);
        assert(hardware == 1 && software == 1 && other_runs == 1);
        psx_request_return_to_lobby(); abort();
    } else abort();
    return 1;
}
static void open_event(CPUState *cpu, uint32_t cls, uint32_t spec, uint32_t callback)
{
    cpu->gpr[9] = 8; cpu->gpr[4] = cls; cpu->gpr[5] = spec;
    cpu->gpr[6] = 0x1000; cpu->gpr[7] = callback;
    assert(hle_bios_dispatch(cpu, HLE_VEC_B0, HLE_VEC_B0));
    assert(cpu->gpr[2] != 0xFFFFFFFFu);
    cpu->gpr[9] = 0x0C; cpu->gpr[4] = cpu->gpr[2];
    assert(hle_bios_dispatch(cpu, HLE_VEC_B0, HLE_VEC_B0));
}
int main(int argc, char **argv)
{
    test_process_init(); delayed = argc > 1 && !strcmp(argv[1], "delayed");
    assert(dc_kernel_activate());
    CPUState cpu = {0}; cpu.read_word = psx_read_word; cpu.write_word = psx_write_word;
    cpu.read_byte = read_byte; cpu.write_byte = write_byte;
    memset(g_test_ram, 0, sizeof g_test_ram);
    psx_write_word(0x108, PCB); psx_write_word(PCB, TCB_A);
    psx_write_word(0x110, TCB_A); psx_write_word(0x114, HLE_TCB_SIZE);
    for (unsigned i = 0; i < 2; ++i) {
        uint32_t tcb = TCB_A + i * 0xC0;
        psx_write_word(tcb, 0x4000); psx_write_word(tcb + 136, i ? OTHER : START);
        psx_write_word(tcb + 148, 0x404); psx_write_word(tcb + 8 + 29 * 4, 0x8001F000);
    }
    cpu.gpr[29] = 0x8001F000; cpu.gpr[31] = RETURN;
    open_event(&cpu, 0xF0000011u, 4, HW); open_event(&cpu, 0xF4000001u, 4, SW);
    open_event(&cpu, 0xF0000011u, 0x8000, HW); open_event(&cpu, 0xF4000001u, 0x8000, SW);
    if (delayed) {
        read_card(&cpu); assert(hle_bios_dispatch(&cpu, HLE_VEC_B0, HLE_VEC_B0));
        assert(cpu.pc == 0 && cpu.gpr[2] == 1 && !hardware && !software);
    }
    psx_scheduler_run(&cpu);
    assert(returns == 1 && hardware == 1 && software == 1);
    for (unsigned i = 0; i < 128; ++i) assert(read_byte(BUFFER + i) == (uint8_t)(i ^ 0x5A));
    final_call = 1; read_card(&cpu);
    psx_dispatch_call(&cpu, 0x800000B0u, RETURN);
    assert(cpu.pc == 0 && cpu.gpr[2] == 0 && cpu.gpr[4] == 0 && cpu.gpr[5] == 7);
    assert(hardware == 2 && software == 2 && returns == 1);
    static DcKernelEventState state; dc_kernel_event_capture(&state);
    for (unsigned i = 0; i < DC_KERNEL_EVENT_CAPACITY; ++i) assert(!state.frames[i].owner);
    puts("PASS: native card completion preserves callbacks, result and delayed-arm ordering across TCB switch");
    return 0;
}
