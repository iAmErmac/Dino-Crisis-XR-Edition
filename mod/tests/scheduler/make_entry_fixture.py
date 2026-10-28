"""Generate a synthetic C execution test through the hook transformer."""

import hashlib
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import hook_migrate


def main(output):
    payload = "{ extern int blocked(uint32_t actor); if (blocked(g4)) { g2 = 0; PSX_SPILL; return; } }"
    source = """void func_80010000(CPUState* cpu)
{
    if (cpu->pc != 0u) {
        uint32_t continuation = cpu->pc; cpu->pc = 0;
        switch (continuation) {
        case 0x80010004u: goto block_80010004;
        default: break;
        }
    }
    debug_server_log_call_entry(0x80010000u);
block_80010000:
    cpu->gpr[3] += 1; /* 0x80010000: 0x24630001 */
block_80010004:
    cpu->gpr[3] += 2; /* 0x80010004: 0x24630002 */
    cpu->pc = cpu->gpr[31];
}
"""
    edit = {
        "id": "H000", "function": "80010000",
        "payload_sha256": hashlib.sha256(payload.encode()).hexdigest(),
        "removed_lines": 0, "inserted_lines": 1,
        "previous_instruction": None,
        "next_instruction": {"pc": "80010000", "opcode": "24630001"},
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
typedef struct { uint32_t gpr[32]; uint32_t pc; } CPUState;
static unsigned predicate_calls, entry_calls;
int blocked(uint32_t actor) { ++predicate_calls; return actor == 7; }
static void debug_server_log_call_entry(uint32_t pc)
{ assert(pc == 0x80010000u); ++entry_calls; }
"""
    suffix = """
int main(void)
{
    test_process_init();
    CPUState cpu = {0};
    cpu.gpr[4] = 7; cpu.gpr[2] = 99; cpu.gpr[31] = 0x80020000u;
    cpu.pc = 0x80010000u;
    func_80010000(&cpu);
    assert(cpu.gpr[2] == 0 && cpu.gpr[3] == 0 && cpu.pc == cpu.gpr[31]);
    assert(predicate_calls == 1 && entry_calls == 1);
    cpu.gpr[4] = 8; cpu.pc = 0x80010000u;
    func_80010000(&cpu);
    assert(cpu.gpr[3] == 3 && cpu.pc == cpu.gpr[31]);
    assert(predicate_calls == 2 && entry_calls == 2);
    cpu.gpr[4] = 7; cpu.pc = 0x80010004u;
    func_80010000(&cpu);
    assert(cpu.gpr[3] == 5 && cpu.pc == cpu.gpr[31]);
    assert(predicate_calls == 2 && entry_calls == 2);
    return 0;
}
"""
    Path(output).write_text(prefix + transformed + suffix, encoding="utf-8")


if __name__ == "__main__":
    main(sys.argv[1])
