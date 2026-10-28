"""Generate a call wrapper check with an escaping pre-callee interrupt poll."""
import hashlib
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import hook_migrate


def main(output):
    source = """void func_80010000(CPUState* cpu)
{
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
    cpu->gpr[4] = 0xDEAD; /* 0x80020000: 0x3404DEAD */
    cpu->pc = cpu->gpr[31];
}
"""
    payload = ("PSX_SPILL; { uint32_t a = cpu->gpr[4], b = cpu->gpr[5], c = cpu->gpr[6]; "
               "func_80020000(cpu); pt_anim_frame(cpu, a, b, c); } PSX_RELOAD;")
    edit = {"id": "H000", "function": "80010000", "payload_sha256": hashlib.sha256(payload.encode()).hexdigest(),
            "previous_instruction": {"pc": "80010008", "opcode": "34A5002C"},
            "next_instruction": {"pc": "8001000C", "opcode": "24630001"}, "replaced_instructions": []}
    old = {"id": "USA11-H000", "function": "80010000", "added": payload}
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        generated = root / "generated"; generated.mkdir()
        (generated / "sample_full_00.c").write_text(source)
        result, changes = hook_migrate.analyze_operation(hook_migrate.GeneratedSources(generated), edit, old)
        if result["disposition"] != "adapted": raise ValueError(result)
        hook_migrate.apply_changes(generated, root / "out", changes)
        transformed = (root / "out/sample_full_00.c").read_text()
    prefix = """#include <assert.h>
#include <setjmp.h>
#include <stdlib.h>
#include "dc_call_state.h"
#include "test_process.h"
static jmp_buf escape;
static unsigned consumed;
uint32_t psx_sched_current_tcb(CPUState *cpu) { (void)cpu; return 0xE100; }
void psx_fatal_halt(const char *reason) { (void)reason; abort(); }
void pt_anim_frame(CPUState *cpu, uint32_t a, uint32_t b, uint32_t c)
{ (void)cpu; assert(a == 11 && b == 44 && c == 33); ++consumed; }
static void psx_check_interrupts_at(CPUState *cpu, uint32_t pc)
{
    DcCallFrame *frame = dc_call_resume(cpu, 0x8001000Cu, DC_CALL_ANIMATION);
    assert(frame->state == DC_CALL_ACTIVE && frame->values[0] == 11);
    cpu->pc = pc;
    longjmp(escape, 1);
}
"""
    suffix = """
int main(void)
{
    test_process_init();
    static CPUState cpu;
    cpu.gpr[4] = 11; cpu.gpr[6] = 33; cpu.gpr[29] = 0x7000;
    if (!setjmp(escape)) { func_80010000(&cpu); assert(!"poll must escape"); }
    assert(cpu.pc == 0x80020000u);
    func_80020000(&cpu);
    func_80010000(&cpu);
    assert(consumed == 1 && cpu.gpr[3] == 1);
    cpu.pc = 0x8001000Cu;
    func_80010000(&cpu);
    assert(consumed == 1);
    return 0;
}
"""
    Path(output).write_text(prefix + transformed + suffix)


if __name__ == "__main__": main(sys.argv[1])
