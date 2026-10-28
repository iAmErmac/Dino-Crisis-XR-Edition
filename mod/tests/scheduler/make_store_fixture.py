"""Generate an executable check for a conditional reload beside a delay slot."""

import hashlib
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import hook_migrate


def main(output):
    payload = "{ extern int blocked(uint32_t actor); if (blocked(g16)) g2 = pgxp_lhu(cpu, g16 + 280, 2); }"
    source = """void func_80010000(CPUState* cpu)
{
    cpu->gpr[3] += 1; /* 0x80010000: 0x24630001 */
    int _bc_80010004 = (cpu->gpr[2] == 0); /* 0x80010004: 0x10400001 */
    { uint32_t _pgxa = cpu->gpr[16] + 280;
      cpu->write_half(_pgxa, (uint16_t)cpu->gpr[2]);
      PGXP_STORE(0xA6020118u, _pgxa, cpu->gpr[2]); } /* 0x80010008: 0xA6020118 */
    cpu->pc = _bc_80010004 ? 0x9004 : 0x9008;
}
"""
    edit = {
        "id": "H000", "function": "80010000",
        "payload_sha256": hashlib.sha256(payload.encode()).hexdigest(),
        "removed_lines": 0, "inserted_lines": 1,
        "previous_instruction": {"pc": "80010000", "opcode": "24630001"},
        "next_instruction": {"pc": "80010008", "opcode": "A6020118"},
        "replaced_instructions": [],
    }
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        path = root / "sample_full_00.c"
        path.write_text(source, encoding="utf-8")
        sources = hook_migrate.GeneratedSources(root)
        report, change = hook_migrate.analyze_operation(sources, edit, {"added": payload})
        if report["disposition"] != "adapted" or change is None:
            raise ValueError(report)
        lines = sources.lines[path.resolve()]
        lines[change[1]:change[2]] = change[3]
        transformed = "".join(lines)
    prefix = """#include <assert.h>
#include <stdint.h>
#include "test_process.h"
typedef struct {
    uint32_t gpr[32]; uint32_t pc;
    uint16_t (*read_half)(uint32_t);
    void (*write_half)(uint32_t, uint16_t);
} CPUState;
static unsigned reads, writes, observations, predicates;
static int miss;
static uint16_t stored;
int blocked(uint32_t actor) { assert(actor == 0x1000); ++predicates; return miss; }
static uint16_t read_half(uint32_t address)
{ assert(address == 0x1118); ++reads; return 0x55AA; }
static void write_half(uint32_t address, uint16_t value)
{ assert(address == 0x1118); ++writes; stored = value; }
static void observe_load(uint32_t instruction, uint32_t address, uint32_t value)
{ assert(instruction == 0x94020000u && address == 0x1118 && value == 0x55AA); ++observations; }
#define PGXP_LOAD(i, a, v) observe_load(i, a, v)
#define PGXP_STORE(i, a, v) ((void)0)
"""
    suffix = """
int main(void)
{
    test_process_init();
    CPUState cpu = {0};
    cpu.read_half = read_half; cpu.write_half = write_half; cpu.gpr[16] = 0x1000;
    miss = 1;
    func_80010000(&cpu);
    assert(stored == 0x55AA && cpu.pc == 0x9004);
    assert(reads == 1 && writes == 1 && observations == 1);
    cpu.gpr[2] = 0; miss = 0;
    func_80010000(&cpu);
    assert(stored == 0 && cpu.pc == 0x9004);
    assert(reads == 1 && writes == 2 && observations == 1);
    cpu.gpr[2] = 7; miss = 1;
    func_80010000(&cpu);
    assert(stored == 0x55AA && cpu.pc == 0x9008);
    assert(reads == 2 && writes == 3 && observations == 2 && predicates == 3);
    return 0;
}
"""
    Path(output).write_text(prefix + transformed + suffix, encoding="utf-8")


if __name__ == "__main__":
    main(sys.argv[1])
