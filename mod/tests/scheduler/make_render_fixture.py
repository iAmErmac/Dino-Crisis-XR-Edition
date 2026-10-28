"""Generate executable checks for conditional CPS calls and loop shortcuts."""
import hashlib
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import hook_migrate


def main(output):
    source = """void func_80010000(CPUState* cpu)
{
    if (cpu->pc == 0x8001000Cu) { cpu->pc = 0; goto block_8001000C; }
    switch (cpu->pc) { case 0x8001000Cu: goto block_8001000C; }
    cpu->gpr[31] = 0x8001000Cu;
    cpu->gpr[5] |= 44; /* 0x80010008: 0x34A5002C */
    psx_check_interrupts_at(cpu, 0x80020000u);
    cpu->pc = 0x80020000u; return;
block_8001000C:
    cpu->gpr[3] += 1; /* 0x8001000C: 0x24630001 */
    cpu->pc = 0;
}
void func_80020000(CPUState* cpu)
{
    cpu->gpr[2] = 7; /* 0x80020000: 0x24020007 */
    cpu->pc = cpu->gpr[31];
}
void func_80030000(CPUState* cpu)
{
    if (cpu->pc == 0x80030004u) goto block_80030004;
    cpu->gpr[23] = cpu->gpr[7]; /* 0x80030000: 0x00E0B821 */
block_80030004:
    cpu->gpr[5] -= 1; /* 0x80030004: 0x24A5FFFF */
    cpu->gpr[3] += 1;
    if (cpu->gpr[5]) goto block_80030004;
block_80030010:
    cpu->pc = 0; /* 0x80030010: 0x00000000 */
}
"""
    payloads = [
        "PSX_SPILL; if (!dc_meshrep_actor(cpu, cpu->gpr[30])) func_80020000(cpu); PSX_RELOAD;",
        "if (dc_room_taken(g4) || dc_meshrep_group(cpu, g16, g4, 0)) goto block_80030010;",
    ]
    edits = []
    for index, (function, previous, following) in enumerate((
        ("80010000", ("80010008", "34A5002C"), ("8001000C", "24630001")),
        ("80030000", ("80030000", "00E0B821"), ("80030004", "24A5FFFF")),
    )):
        edits.append({
            "id": f"H{index:03}", "function": function,
            "payload_sha256": hashlib.sha256(payloads[index].encode()).hexdigest(),
            "previous_instruction": dict(zip(("pc", "opcode"), previous)),
            "next_instruction": dict(zip(("pc", "opcode"), following)),
            "replaced_instructions": [],
        })
    catalog = {"schema": 1, "source_sha256": "0" * 64,
               "patch_sha256": "1" * 64, "edit_count": 2, "edits": edits}
    old = [{"id": f"USA11-H{i:03}", "function": edits[i]["function"], "added": payload}
           for i, payload in enumerate(payloads)]
    hook_migrate.validate_inputs(catalog, old)
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        generated = root / "generated"; generated.mkdir()
        (generated / "sample_full_00.c").write_text(source, encoding="utf-8")
        report, changes = hook_migrate.analyze(catalog, old, hook_migrate.GeneratedSources(generated))
        if report["counts"]["adapted"] != 2: raise ValueError(report)
        hook_migrate.apply_changes(generated, root / "output", changes)
        transformed = (root / "output/sample_full_00.c").read_text(encoding="utf-8")
    prefix = """#include <assert.h>
#include <stdint.h>
#include "test_process.h"
typedef struct { uint32_t gpr[32]; uint32_t pc; } CPUState;
static int replaced, room, group;
static unsigned actor_calls, group_calls;
static uint32_t irq_pc;
static void psx_check_interrupts_at(CPUState *cpu, uint32_t pc) { (void)cpu; irq_pc = pc; }
static int dc_meshrep_actor(CPUState *cpu, uint32_t ctx) { (void)cpu; assert(ctx == 0x1234); ++actor_calls; return replaced; }
static int dc_room_taken(uint32_t ptr) { (void)ptr; return room; }
static int dc_meshrep_group(CPUState *cpu, uint32_t hdr, uint32_t arr, int lit)
{ (void)cpu; (void)hdr; (void)arr; assert(lit == 0); ++group_calls; return group; }
"""
    suffix = """
int main(void)
{
    test_process_init();
    for (int mode = 0; mode < 2; ++mode) {
        CPUState cpu = {0}; cpu.gpr[30] = 0x1234; cpu.gpr[2] = 99; replaced = mode;
        func_80010000(&cpu);
        assert(irq_pc == (mode ? 0x8001000Cu : 0x80020000u));
        assert(cpu.pc == irq_pc);
        if (!mode) func_80020000(&cpu);
        func_80010000(&cpu);
        assert(cpu.pc == 0 && cpu.gpr[3] == 1 && cpu.gpr[2] == (mode ? 99u : 7u));
    }
    assert(actor_calls == 2);
    for (int mode = 0; mode < 4; ++mode) {
        CPUState cpu = {0}; cpu.gpr[5] = 3; group_calls = 0;
        room = mode == 1; group = mode == 2;
        if (mode == 3) cpu.pc = 0x80030004u;
        func_80030000(&cpu);
        assert(cpu.gpr[3] == (mode == 1 || mode == 2 ? 0u : 3u));
        assert(group_calls == (mode == 0 || mode == 2 ? 1u : 0u));
    }
    return 0;
}
"""
    Path(output).write_text(prefix + transformed + suffix, encoding="utf-8")


if __name__ == "__main__": main(sys.argv[1])
