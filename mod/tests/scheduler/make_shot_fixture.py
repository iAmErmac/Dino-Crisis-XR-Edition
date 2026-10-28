"""Generate a synthetic deferred-shot CPS fixture for matcher and native tests."""

import hashlib
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import hook_migrate


SOURCE = r"""void func_800505C8(CPUState* cpu)
{
    uint32_t resumed = cpu->pc; cpu->pc = 0;
    switch (resumed) {
    case 0x8005076Cu: goto block_8005076C;
    default: break;
    }
    debug_server_log_call_entry(0x800505C8u);
    cpu->gpr[29] -= 120u; /* 0x800505C8: 0x27BDFF88 */
    fixture_store_word(cpu, cpu->gpr[29] + 116u, cpu->gpr[31]);
    cpu->gpr[22] = cpu->gpr[4]; /* 0x800505D0: 0x0080B021 */
    cpu->gpr[30] = cpu->gpr[29] + 32u;
    cpu->gpr[19] = 0x5Au;
block_8005076C:
    fixture_store_byte(cpu, cpu->gpr[30] + 140u, (uint8_t)cpu->gpr[19]); /* 0x8005076C: 0xA3D3008C */
    cpu->gpr[31] = fixture_load_word(cpu, cpu->gpr[29] + 116u); /* 0x80050770: 0x8FBF0074 */
    cpu->gpr[29] += 120u; /* 0x8005079C: 0x27BD0078 */
    cpu->pc = cpu->gpr[31]; return;
}

void func_80080DA0(CPUState* cpu)
{
    assert(cpu->gpr[4] == 0x800B92ACu);
    assert(cpu->gpr[5] == 0x800B7AECu);
    assert(cpu->gpr[6] == 5u);
    assert(cpu->gpr[31] == 0x8005076Cu);
    uint32_t continuation = cpu->gpr[31];
    cpu->gpr[2] = 0;
    cpu->gpr[4] = 0xDEAD0004u;
    cpu->gpr[5] = 0xDEAD0005u;
    cpu->gpr[6] = 0xDEAD0006u;
    cpu->gpr[7] = 0xDEAD0007u;
    cpu->gpr[8] = 0xDEAD0008u;
    cpu->gpr[31] = 0xDEAD0031u;
    cpu->hi = 0xDEAD1111u;
    cpu->lo = 0xDEAD2222u;
    ++visibility_calls;
    cpu->pc = continuation;
}

void func_8002E7CC(CPUState* cpu)
{
    assert(cpu->gpr[4] == 0x800B7ACCu);
    assert(cpu->gpr[31] == 0x8005076Cu);
    uint32_t continuation = cpu->gpr[31];
    cpu->gpr[4] = 0xBEEF0004u;
    cpu->gpr[7] = 0xBEEF0007u;
    cpu->gpr[8] = 0xBEEF0008u;
    cpu->gpr[31] = 0xBEEF0031u;
    cpu->hi = 0xBEEF1111u;
    cpu->lo = 0xBEEF2222u;
    ++blood_calls;
    cpu->pc = continuation;
}
"""

ENTRY_PAYLOAD = "{ extern void dc_shot_list_begin(uint32_t player, uint32_t params); dc_shot_list_begin(g4,g5); }"
CALLBACK_PAYLOAD = "PSX_SPILL; { extern void dc_shot_list_fix(CPUState* cpu, uint32_t player); dc_shot_list_fix(cpu,cpu->gpr[22]); } PSX_RELOAD;"


def _edit(index, function, payload):
    return {
        "id": f"H{index:03d}",
        "function": function,
        "old_line": index + 1,
        "removed_lines": 0,
        "inserted_lines": len(payload.splitlines()),
        "payload_sha256": hashlib.sha256(payload.encode("utf-8")).hexdigest(),
        "previous_instruction": None,
        "next_instruction": None,
        "replaced_instructions": [],
    }


def edits():
    return [_edit(49, "800505C8", ENTRY_PAYLOAD), _edit(50, "800505C8", CALLBACK_PAYLOAD)]


def old_hooks():
    return [
        {"id": "USA11-H049", "function": "800505C8", "at": 49, "drop": 0, "added": ENTRY_PAYLOAD},
        {"id": "USA11-H050", "function": "800505C8", "at": 50, "drop": 0, "added": CALLBACK_PAYLOAD},
    ]


CATALOG = {
    "schema": 1,
    "source_sha256": "0" * 64,
    "patch_sha256": "1" * 64,
    "edit_count": 2,
    "edits": edits(),
}


def transformed_source():
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        generated = root / "generated"
        generated.mkdir()
        (generated / "sample_full_00.c").write_text(SOURCE, encoding="utf-8")
        sources = hook_migrate.GeneratedSources(generated)
        report, changes = hook_migrate.analyze(CATALOG, old_hooks(), sources)
        if report["counts"]["adapted"] != 2 or len(changes) != 3:
            raise ValueError(report)
        output = root / "out"
        hook_migrate.apply_changes(generated, output, changes)
        return (output / "sample_full_00.c").read_text(encoding="utf-8")


PREFIX = r"""#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "cpu_state.h"
#include "dc_call_state.h"
#include "dc_shot.h"
#include "test_process.h"

#define RETURN_PC 0x80040000u
#define ENTRY_PC 0x800505C8u
#define RESUME_PC 0x8005076Cu
#define VISIBILITY_PC 0x80080DA0u
#define BLOOD_PC 0x8002E7CCu

static uint8_t guest_ram[0x8000];
static int shot_enabled;
static unsigned prepare_calls, commit_calls, blood_actor_calls;
static unsigned visibility_calls, blood_calls, poll_calls;
static unsigned store_calls, rng_get_calls, rng_set_calls, pool_checks;
static uint32_t rng_seed;

static void fixture_store_word(CPUState *cpu, uint32_t address, uint32_t value)
{
    (void)cpu;
    assert(address + 3u < sizeof guest_ram);
    guest_ram[address] = (uint8_t)value;
    guest_ram[address + 1u] = (uint8_t)(value >> 8);
    guest_ram[address + 2u] = (uint8_t)(value >> 16);
    guest_ram[address + 3u] = (uint8_t)(value >> 24);
}

static uint32_t fixture_load_word(CPUState *cpu, uint32_t address)
{
    (void)cpu;
    assert(address + 3u < sizeof guest_ram);
    return (uint32_t)guest_ram[address] |
           ((uint32_t)guest_ram[address + 1u] << 8) |
           ((uint32_t)guest_ram[address + 2u] << 16) |
           ((uint32_t)guest_ram[address + 3u] << 24);
}

static void fixture_store_byte(CPUState *cpu, uint32_t address, uint8_t value)
{
    (void)cpu;
    assert(address < sizeof guest_ram);
    ++store_calls;
    guest_ram[address] = value;
}

int dc_shot_prepare(uint32_t player, uint32_t parameters, DcShotState *state)
{
    assert(player == 0x800B928Cu && parameters == 0x80010000u);
    ++prepare_calls;
    memset(state, 0, sizeof *state);
    if (!shot_enabled) return 0;
    state->enabled = 1;
    state->visibility = 1;
    state->actor = 0x000B7ACCu;
    state->candidate = 0;
    state->count = 1;
    return 1;
}

void dc_shot_commit(DcShotState *state, uint32_t blocked)
{
    assert(state->enabled == 1 && blocked == 0);
    ++commit_calls;
    state->enabled = 1;
    state->victim_count = 1;
    state->victims[0] = 0;
    state->blood_event = 1;
}

int dc_shot_blood_actor(int slot, uint32_t *actor)
{
    assert(slot == 0 && actor != NULL);
    ++blood_actor_calls;
    *actor = 0x000B7ACCu;
    return 1;
}

uint32_t dc_rng_seed_get(void)
{
    ++rng_get_calls;
    return rng_seed;
}

void dc_rng_seed_set(uint32_t seed)
{
    assert(seed == 0x1357u);
    ++rng_set_calls;
    rng_seed = seed;
}

int dc_blood_mult(void)
{
    return 2;
}

int dc_blood_pool_room(void)
{
    ++pool_checks;
    return 1;
}

uint8_t *memory_get_ram_ptr(void)
{
    return guest_ram;
}

void spu_xpage_capture(int page)
{
    (void)page;
    abort();
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
    assert(pc == ENTRY_PC);
}

void psx_check_interrupts_at(CPUState *cpu, uint32_t target)
{
    assert(target == VISIBILITY_PC || target == BLOOD_PC);
    ++poll_calls;
    cpu->pc = target;
}

void func_800505C8(CPUState *cpu);
void func_80080DA0(CPUState *cpu);
void func_8002E7CC(CPUState *cpu);

static void init_cpu(CPUState *cpu)
{
    memset(cpu, 0, sizeof *cpu);
    cpu->pc = ENTRY_PC;
    cpu->gpr[29] = 0x7000u;
    cpu->gpr[4] = 0x800B928Cu;
    cpu->gpr[5] = 0x80010000u;
    cpu->gpr[6] = 0x11112222u;
    cpu->gpr[7] = 0x33334444u;
    cpu->gpr[8] = 0x55556666u;
    cpu->gpr[31] = RETURN_PC;
    cpu->hi = 0x12345678u;
    cpu->lo = 0x9ABCDEF0u;
}

static void run_until_done(CPUState *cpu)
{
    for (unsigned step = 0; step < 32 && cpu->pc != RETURN_PC; ++step) {
        switch (cpu->pc) {
        case ENTRY_PC:
        case RESUME_PC:
            func_800505C8(cpu);
            break;
        case VISIBILITY_PC:
            func_80080DA0(cpu);
            break;
        case BLOOD_PC:
            func_8002E7CC(cpu);
            break;
        default:
            abort();
        }
    }
    assert(cpu->pc == RETURN_PC);
}

static void reset_fixture(void)
{
    memset(guest_ram, 0, sizeof guest_ram);
    shot_enabled = 0;
    prepare_calls = commit_calls = blood_actor_calls = 0;
    visibility_calls = blood_calls = poll_calls = 0;
    store_calls = rng_get_calls = rng_set_calls = pool_checks = 0;
    rng_seed = 0x1357u;
    dc_call_reset();
}

static void assert_restored(const CPUState *cpu)
{
    assert(cpu->gpr[4] == 0x800B928Cu && cpu->gpr[5] == 0x80010000u);
    assert(cpu->gpr[6] == 0x11112222u && cpu->gpr[7] == 0x33334444u);
    assert(cpu->gpr[8] == 0x55556666u);
    assert(cpu->gpr[31] == RETURN_PC && cpu->gpr[29] == 0x7000u);
    assert(cpu->hi == 0x12345678u && cpu->lo == 0x9ABCDEF0u);
}

static void test_disabled_path(void)
{
    CPUState cpu;
    reset_fixture();
    init_cpu(&cpu);
    run_until_done(&cpu);
    assert(prepare_calls == 1 && commit_calls == 0 && blood_actor_calls == 0);
    assert(visibility_calls == 0 && blood_calls == 0 && poll_calls == 0);
    assert(store_calls == 1 && rng_get_calls == 0 && rng_set_calls == 0);
    assert_restored(&cpu);
}

static void test_visibility_and_blood_resume(void)
{
    CPUState cpu;
    reset_fixture();
    shot_enabled = 1;
    init_cpu(&cpu);
    assert(cpu.pc == ENTRY_PC && cpu.gpr[29] == 0x7000u);
    run_until_done(&cpu);
    assert(prepare_calls == 1 && commit_calls == 1 && blood_actor_calls == 1);
    assert(visibility_calls == 1 && blood_calls == 2 && poll_calls == 3);
    assert(store_calls == 1);
    assert(rng_get_calls == 1 && rng_set_calls == 1 && rng_seed == 0x1357u);
    assert(pool_checks >= 2);
    assert_restored(&cpu);
}

int main(void)
{
    test_process_init();
    test_disabled_path();
    test_visibility_and_blood_resume();
    return 0;
}
"""


def main(output):
    Path(output).write_text(PREFIX + transformed_source() + "\n", encoding="utf-8")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: make_shot_fixture.py OUTPUT")
    main(sys.argv[1])
