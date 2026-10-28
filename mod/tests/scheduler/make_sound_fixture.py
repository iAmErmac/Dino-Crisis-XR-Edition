"""Generate an entry/return sound scope check with escaping interrupt polls."""
import hashlib
import sys
import tempfile
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import hook_migrate

SOURCE = """void func_80010000(CPUState* cpu)
{
    switch (cpu->pc) { case 0x80010010u: goto block_80010010; }
    debug_server_log_call_entry(0x80010000u);
block_80010000:
    cpu->gpr[29] -= 32; /* 0x80010000: 0x27BDFFE0 */
    cpu->gpr[16] = cpu->gpr[31]; /* 0x80010004: 0x03E08021 */
    cpu->gpr[31] = 0x80010010u;
    psx_check_interrupts_at(cpu, 0x80020000u);
    cpu->pc = 0x80020000u; return;
block_80010010:
    cpu->gpr[31] = cpu->gpr[16]; /* 0x80010010: 0x0200F821 */
    uint32_t _jt_80010014 = cpu->gpr[31];
    cpu->gpr[29] += 32; /* 0x80010018: 0x27BD0020 */
    psx_check_interrupts_at(cpu, _jt_80010014);
    cpu->pc = _jt_80010014; return;
}
void func_80020000(CPUState* cpu)
{
    cpu->gpr[4] = 0xDEAD; /* 0x80020000: 0x3404DEAD */
    cpu->pc = cpu->gpr[31];
}
"""
PAYLOAD = ("{ extern int dc_fk_sound(CPUState *cpu); if (dc_fk_sound(cpu)) { PSX_RELOAD; PSX_SPILL; return; } } "
           "{ extern void dc_shot_effect_note(uint32_t a0); dc_shot_effect_note(g4); }")

def edit():
    return {"id": "H000", "function": "80010000", "payload_sha256": hashlib.sha256(PAYLOAD.encode()).hexdigest(),
            "next_instruction": {"pc": "80010000", "opcode": "27BDFFE0"}, "replaced_instructions": []}

def main(output):
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory); generated = root / "generated"; generated.mkdir()
        (generated / "sample_full_00.c").write_text(SOURCE)
        old = {"id": "USA11-H000", "function": "80010000", "added": PAYLOAD}
        result, changes = hook_migrate.analyze_operation(hook_migrate.GeneratedSources(generated), edit(), old)
        if result["disposition"] != "adapted": raise ValueError(result)
        hook_migrate.apply_changes(generated, root / "out", changes)
        transformed = (root / "out/sample_full_00.c").read_text()
        result, changes = hook_migrate.analyze_operation(hook_migrate.GeneratedSources(root / "out"), edit(), old)
        if result["disposition"] != "retained" or changes: raise ValueError(result)
    prefix = """#include <assert.h>
#include <setjmp.h>
#include <stdlib.h>
#include "dc_call_state.h"
#include "test_process.h"
static uint32_t ram[0x200000 / 4];
static jmp_buf escape;
static unsigned notes, preparations, entry_logs;
static int capture_page;
uint8_t *memory_get_ram_ptr(void) { return (uint8_t *)ram; }
uint32_t psx_sched_current_tcb(CPUState *cpu) { (void)cpu; return 0xE100; }
void psx_fatal_halt(const char *reason) { (void)reason; abort(); }
void spu_xpage_capture(int page) { assert(page == 0 || page == 3); capture_page = page; }
int dc_fk_sound_prepare(CPUState *cpu, DcSoundSwap *swap)
{
    (void)cpu; ++preparations;
    swap->entry_address = 0x1FEA00; swap->table_address = 0x9E538;
    swap->entry_word = ram[0x1FEA00 / 4]; swap->table_word = ram[0x9E538 / 4]; swap->page = 3;
    ram[0x1FEA00 / 4] = 0xABCD; ram[0x9E538 / 4] = 0x1234;
    dc_sound_capture_page(3); return 1;
}
void dc_shot_effect_note(uint32_t actor) { assert(actor == 11); ++notes; }
static void debug_server_log_call_entry(uint32_t pc) { assert(pc == 0x80010000u); ++entry_logs; }
static void psx_check_interrupts_at(CPUState *cpu, uint32_t pc)
{
    if (pc == 0x80020000u) assert(capture_page == 3 && dc_call_sound_active());
    else {
        assert(pc == 0x80040000u && !capture_page && !dc_call_sound_active());
        assert(ram[0x1FEA00 / 4] == 0x1122 && ram[0x9E538 / 4] == 0x3344);
    }
    cpu->pc = pc; longjmp(escape, 1);
}
"""
    suffix = """
int main(void)
{
    test_process_init(); static CPUState cpu;
    ram[0x1FEA00 / 4] = 0x1122; ram[0x9E538 / 4] = 0x3344;
    cpu.gpr[4] = 11; cpu.gpr[29] = 0x7000; cpu.gpr[31] = 0x80040000u;
    if (!setjmp(escape)) { func_80010000(&cpu); assert(!"callee poll must escape"); }
    assert(cpu.pc == 0x80020000u);
    func_80020000(&cpu);
    if (!setjmp(escape)) { func_80010000(&cpu); assert(!"return poll must escape"); }
    assert(cpu.pc == 0x80040000u && cpu.gpr[29] == 0x7000);
    assert(notes == 1 && preparations == 1 && entry_logs == 1);
    return 0;
}
"""
    Path(output).write_text(prefix + transformed + suffix)

if __name__ == "__main__": main(sys.argv[1])
