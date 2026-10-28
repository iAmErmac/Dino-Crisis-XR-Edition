"""Generate a synthetic attachment function and an executable CPS-run test."""

import hashlib
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import hook_migrate


SOURCE = r"""void func_80010000(CPUState* cpu)
{
    switch (cpu->pc) {
    case 0x80010014u: goto block_80010014;
    case 0x80010024u: goto block_80010024;
    default: break;
    }
    debug_server_log_call_entry(0x80010000u);
    cpu->gpr[23] = cpu->gpr[4]; /* 0x80010000: 0x0080B821 */
    /* 0x80010004: 0x8EE40000 */
    cpu->gpr[31] = 0x80010014u;
    /* nop */ /* 0x80010010: 0x00000000 */
    cpu->pc = 0x80075484u; return;
block_80010014:
    cpu->gpr[4] = 0x1111u; /* 0x80010014: 0x8EE40004 */
    cpu->gpr[31] = 0x80010024u;
    /* nop */ /* 0x80010020: 0x00000000 */
    cpu->pc = 0x8007511Cu; return;
block_80010024:
    cpu->gpr[1] = 0x1F800000u; /* 0x80010024: 0x3C011F80 */
    cpu->pc = 0; return;
}

void func_80075484(CPUState* cpu)
{
    ++triangle_calls;
    cpu->pc = cpu->gpr[31];
}

void func_8007511C(CPUState* cpu)
{
    ++quad_calls;
    cpu->pc = cpu->gpr[31];
}
"""

PAYLOAD = r"""    { const int dc_att_ = dc_meshrep_attach(cpu, g23, g16, g30);
      if (dc_att_ == 2) { PSX_SPILL; dc_meshrep_attach_draw(cpu, g23); PSX_RELOAD; }
      if (dc_att_) goto block_80010024; }"""


def edit():
    return {
        "id": "H111",
        "function": "80010000",
        "payload_sha256": hashlib.sha256(PAYLOAD.encode("utf-8")).hexdigest(),
        "previous_instruction": {"pc": "80010000", "opcode": "0080B821"},
        "next_instruction": {"pc": "80010004", "opcode": "8EE40000"},
        "replaced_instructions": [],
    }


def transformed_source():
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        generated = root / "generated"
        generated.mkdir()
        (generated / "sample_full_00.c").write_text(SOURCE, encoding="utf-8")
        old = {"id": "USA11-H111", "function": "80010000", "added": PAYLOAD}
        sources = hook_migrate.GeneratedSources(generated)
        result, changes = hook_migrate.analyze_operation(sources, edit(), old)
        if result["disposition"] != "adapted":
            raise ValueError(result)
        output = root / "out"
        hook_migrate.apply_changes(generated, output, changes)
        return (output / "sample_full_00.c").read_text(encoding="utf-8")


PREFIX = r"""#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "cpu_state.h"
#include "dc_call_state.h"
#include "dc_mesh_runs.h"
#include "test_process.h"

static int attachment_result;
static unsigned attach_calls, run_calls, triangle_calls, quad_calls;
static unsigned forget_calls, poll_calls;
static uint32_t captured_header, last_poll_target;
static DcMeshRuns captured_runs;

int dc_meshrep_attach(CPUState *cpu, uint32_t header, uint32_t node, uint32_t context)
{
    (void)cpu;
    assert(header == 0xABCDu && node == 0x1234u && context == 0x5678u);
    ++attach_calls;
    return attachment_result;
}

void dc_meshrep_attach_runs(uint32_t header, DcMeshRuns *runs)
{
    ++run_calls;
    captured_header = header;
    memset(runs, 0, sizeof *runs);
    runs->base[0] = 0x10000u;
    runs->count[0] = 4;
    runs->cut[0] = 1ull << 1;
    runs->base[1] = 0x20000u;
    runs->count[1] = 3;
    runs->cut[1] = 1ull << 2;
    captured_runs = *runs;
}

uint8_t *memory_get_ram_ptr(void) { abort(); }
void spu_xpage_capture(int page) { (void)page; abort(); }
uint32_t psx_sched_current_tcb(CPUState *cpu) { (void)cpu; return 0xE100u; }
void psx_fatal_halt(const char *reason) { (void)reason; abort(); }
void psx_pgxp_forget_register(uint32_t reg)
{
    assert(reg == 4u || reg == 5u || reg == 31u);
    ++forget_calls;
}

void debug_server_log_call_entry(uint32_t pc)
{
    assert(pc == 0x80010000u);
}

void psx_check_interrupts_at(CPUState *cpu, uint32_t target)
{
    assert(target == 0x80075484u || target == 0x8007511Cu);
    ++poll_calls;
    last_poll_target = target;
    cpu->pc = target;
}

void func_80010000(CPUState *cpu);
void func_80075484(CPUState *cpu);
void func_8007511C(CPUState *cpu);

static void run_until_done(CPUState *cpu)
{
    for (unsigned step = 0; step < 64 && cpu->pc; ++step) {
        switch (cpu->pc) {
        case 0x80010000u:
        case 0x80010014u:
        case 0x80010024u:
            func_80010000(cpu);
            break;
        case 0x80075484u:
            func_80075484(cpu);
            break;
        case 0x8007511Cu:
            func_8007511C(cpu);
            break;
        default:
            abort();
        }
    }
    assert(cpu->pc == 0);
}

static void reset_observations(void)
{
    attachment_result = 0;
    attach_calls = run_calls = triangle_calls = quad_calls = 0;
    forget_calls = poll_calls = 0;
    captured_header = last_poll_target = 0;
    memset(&captured_runs, 0, sizeof captured_runs);
    dc_call_reset();
}

static void test_normal_path(void)
{
    CPUState cpu = {0};
    reset_observations();
    cpu.pc = 0x80010000u;
    cpu.gpr[29] = 0x7000u;
    cpu.gpr[4] = 0xABCDu;
    cpu.gpr[16] = 0x1234u;
    cpu.gpr[30] = 0x5678u;
    run_until_done(&cpu);
    assert(attach_calls == 1 && run_calls == 0);
    assert(triangle_calls == 1 && quad_calls == 1);
    assert(poll_calls == 0 && forget_calls == 0);
}

static void test_replaced_path(void)
{
    CPUState cpu = {0};
    reset_observations();
    attachment_result = 1;
    cpu.pc = 0x80010000u;
    cpu.gpr[29] = 0x7000u;
    cpu.gpr[4] = 0xABCDu;
    cpu.gpr[16] = 0x1234u;
    cpu.gpr[30] = 0x5678u;
    run_until_done(&cpu);
    assert(attach_calls == 1 && run_calls == 0);
    assert(triangle_calls == 0 && quad_calls == 0);
    assert(poll_calls == 0 && forget_calls == 0);
}

static void test_selected_runs(void)
{
    CPUState cpu = {0};
    reset_observations();
    attachment_result = 2;
    cpu.pc = 0x80010000u;
    cpu.gpr[29] = 0x7000u;
    cpu.gpr[4] = 0xABCDu;
    cpu.gpr[16] = 0x1234u;
    cpu.gpr[30] = 0x5678u;
    run_until_done(&cpu);
    assert(attach_calls == 1 && run_calls == 1);
    assert(captured_header == 0xABCDu);
    assert(captured_runs.count[0] == 4 && captured_runs.count[1] == 3);
    assert(triangle_calls == 2 && quad_calls == 1);
    assert(poll_calls == 3 && last_poll_target == 0x8007511Cu);
    assert(forget_calls == 9);
}

int main(void)
{
    test_process_init();
    test_normal_path();
    test_replaced_path();
    test_selected_runs();
    return 0;
}
"""


def main(output):
    Path(output).write_text(PREFIX + transformed_source() + "\n", encoding="utf-8")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: make_mesh_fixture.py OUTPUT")
    main(sys.argv[1])
