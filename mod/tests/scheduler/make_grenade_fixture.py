"""Generate a synthetic deferred-grenade entry fixture for native execution."""

import hashlib
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import hook_migrate


SOURCE = r"""void func_80072C84(CPUState* cpu)
{
    uint32_t resumed = cpu->pc; cpu->pc = 0;
    switch (resumed) {
    case 0x80072C88u: goto block_80072C88;
    default: break;
    }
    debug_server_log_call_entry(0x80072C84u);
    cpu->gpr[6] = 0x1F800000u; /* 0x80072C84: 0x3C061F80 */
block_80072C88:
    cpu->pc = 0;
}

void func_80046330(CPUState* cpu)
{
    assert(cpu->gpr[4] == 0x80012000u);
    assert(cpu->gpr[5] == 1u);
    assert(cpu->gpr[6] == 1u);
    assert(cpu->gpr[31] == 0x80072C84u);
    uint32_t continuation = cpu->gpr[31];
    cpu->gpr[4] = 0xDEAD0004u;
    cpu->gpr[5] = 0xDEAD0005u;
    cpu->gpr[6] = 0xDEAD0006u;
    cpu->gpr[7] = 0xDEAD0007u;
    cpu->gpr[31] = 0xDEAD0031u;
    cpu->hi = 0xDEAD1111u;
    cpu->lo = 0xDEAD2222u;
    ++effect_calls;
    cpu->pc = continuation;
}
"""

PAYLOAD = "PSX_SPILL;pt_anim_aimoff_draw_enter(cpu);PSX_RELOAD;"


def edit():
    return {
        "id": "H077",
        "function": "80072C84",
        "payload_sha256": hashlib.sha256(PAYLOAD.encode("utf-8")).hexdigest(),
        "previous_instruction": None,
        "next_instruction": {"pc": "80072C84", "opcode": "3C061F80"},
        "replaced_instructions": [],
    }


def transformed_source():
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        generated = root / "generated"
        generated.mkdir()
        (generated / "sample_full_00.c").write_text(SOURCE, encoding="utf-8")
        old = {"id": "USA11-H077", "function": "80072C84", "added": PAYLOAD}
        sources = hook_migrate.GeneratedSources(generated)
        result, changes = hook_migrate.analyze_operation(sources, edit(), old)
        if result["disposition"] != "adapted" or changes is None:
            raise ValueError(result)
        output = root / "out"
        hook_migrate.apply_changes(generated, output, [changes])
        return (output / "sample_full_00.c").read_text(encoding="utf-8")


PREFIX = r"""#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "cpu_state.h"
#include "dc_call_state.h"
#include "test_process.h"

static uint32_t grenade_request;
static uint32_t expected_object;
static unsigned grenade_take_calls, grenade_complete_calls;
static unsigned effect_calls, pose_calls, poll_calls;
static uint32_t pose_gpr4, pose_gpr5, pose_gpr6, pose_gpr7, pose_gpr31;
static uint32_t pose_hi, pose_lo, pose_pc;

uint32_t dc_grenade_take(void)
{
    ++grenade_take_calls;
    uint32_t object = grenade_request;
    grenade_request = 0;
    return object;
}

void dc_grenade_complete(uint32_t object)
{
    assert(object == expected_object);
    ++grenade_complete_calls;
}

void pt_anim_aimoff_draw_enter(CPUState *cpu)
{
    assert(cpu->gpr[4] == 0x11112222u);
    assert(cpu->gpr[5] == 0x33334444u);
    assert(cpu->gpr[6] == 0x55556666u);
    assert(cpu->gpr[7] == 0x77778888u);
    assert(cpu->gpr[31] == 0x80071000u);
    assert(cpu->hi == 0x12345678u);
    assert(cpu->lo == 0x9ABCDEF0u);
    assert(cpu->pc == 0u);
    pose_gpr4 = cpu->gpr[4];
    pose_gpr5 = cpu->gpr[5];
    pose_gpr6 = cpu->gpr[6];
    pose_gpr7 = cpu->gpr[7];
    pose_gpr31 = cpu->gpr[31];
    pose_hi = cpu->hi;
    pose_lo = cpu->lo;
    pose_pc = cpu->pc;
    ++pose_calls;
}

uint32_t psx_sched_current_tcb(CPUState *cpu)
{
    (void)cpu;
    return 0xE100u;
}

void psx_fatal_halt(const char *reason)
{
    (void)reason;
    abort();
}

void debug_server_log_call_entry(uint32_t pc)
{
    assert(pc == 0x80072C84u);
}

void psx_check_interrupts_at(CPUState *cpu, uint32_t target)
{
    assert(target == 0x80046330u);
    ++poll_calls;
    cpu->pc = target;
}

uint8_t *memory_get_ram_ptr(void) { abort(); }
void spu_xpage_capture(int page) { (void)page; abort(); }

void func_80072C84(CPUState *cpu);
void func_80046330(CPUState *cpu);

static void init_cpu(CPUState *cpu)
{
    memset(cpu, 0, sizeof *cpu);
    cpu->pc = 0x80072C84u;
    cpu->gpr[29] = 0x7000u;
    cpu->gpr[4] = 0x11112222u;
    cpu->gpr[5] = 0x33334444u;
    cpu->gpr[6] = 0x55556666u;
    cpu->gpr[7] = 0x77778888u;
    cpu->gpr[31] = 0x80071000u;
    cpu->hi = 0x12345678u;
    cpu->lo = 0x9ABCDEF0u;
}

static void run_until_done(CPUState *cpu)
{
    for (unsigned step = 0; step < 8 && cpu->pc; ++step) {
        switch (cpu->pc) {
        case 0x80072C84u:
            func_80072C84(cpu);
            break;
        case 0x80046330u:
            func_80046330(cpu);
            break;
        default:
            abort();
        }
    }
    assert(cpu->pc == 0);
}

static void reset_observations(void)
{
    grenade_request = 0;
    expected_object = 0;
    grenade_take_calls = grenade_complete_calls = 0;
    effect_calls = pose_calls = poll_calls = 0;
    pose_gpr4 = pose_gpr5 = pose_gpr6 = pose_gpr7 = pose_gpr31 = 0;
    pose_hi = pose_lo = pose_pc = 0;
    dc_call_reset();
}

static void test_no_request(void)
{
    CPUState cpu;
    reset_observations();
    init_cpu(&cpu);
    run_until_done(&cpu);
    assert(grenade_take_calls == 1 && grenade_complete_calls == 0);
    assert(effect_calls == 0 && poll_calls == 0 && pose_calls == 1);
    assert(pose_gpr4 == 0x11112222u && pose_gpr31 == 0x80071000u);
    assert(pose_hi == 0x12345678u && pose_lo == 0x9ABCDEF0u && pose_pc == 0u);
}

static void test_one_deferred_request(void)
{
    CPUState cpu;
    reset_observations();
    expected_object = grenade_request = 0x12000u;
    init_cpu(&cpu);
    run_until_done(&cpu);
    assert(grenade_take_calls == 1 && grenade_complete_calls == 1);
    assert(effect_calls == 1 && poll_calls == 1 && pose_calls == 1);
    assert(pose_gpr4 == 0x11112222u && pose_gpr5 == 0x33334444u);
    assert(pose_gpr6 == 0x55556666u && pose_gpr7 == 0x77778888u);
    assert(pose_gpr31 == 0x80071000u);
    assert(pose_hi == 0x12345678u && pose_lo == 0x9ABCDEF0u && pose_pc == 0u);
    assert(cpu.gpr[4] == 0x11112222u && cpu.gpr[5] == 0x33334444u);
    assert(cpu.gpr[31] == 0x80071000u);
    assert(cpu.hi == 0x12345678u && cpu.lo == 0x9ABCDEF0u);
}

int main(void)
{
    test_process_init();
    test_no_request();
    test_one_deferred_request();
    return 0;
}
"""


def main(output):
    Path(output).write_text(PREFIX + transformed_source() + "\n", encoding="utf-8")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: make_grenade_fixture.py OUTPUT")
    main(sys.argv[1])
