#include "dc_creature_blood.h"
#include "dc_call_state.h"
#include "dc_fk_policy.h"
#include "dc_shot.h"
#include "pgxp_hooks.h"
#include <stdlib.h>

extern uint8_t *memory_get_ram_ptr(void);
extern int dc_recent_shot_victim(int slot);
extern int dc_blood_mult(void), dc_blood_pool_room(void);
extern uint32_t dc_rng_seed_get(void);
extern void dc_rng_seed_set(uint32_t seed);
extern uint32_t g_debug_last_store_pc;
extern void psx_fatal_halt(const char *reason);

static int creature_enabled = -1, foreign_enabled;
static _Noreturn void fail(const char *reason) { psx_fatal_halt(reason); abort(); }

int dc_creature_blood_defer(CPUState *cpu, uint32_t pc, uint32_t instruction,
    uint32_t address, uint16_t value)
{
    if (creature_enabled < 0) {
        const char *e = getenv("DC_CREATURE_BLOOD");
        creature_enabled = !(e && e[0] == '0');
        e = getenv("DC_FK_BLOOD"); foreign_enabled = e && e[0] == '1';
    }
    uint32_t phys = address & 0x1FFFFFFFu;
    if ((!creature_enabled && !foreign_enabled) || phys < 0xB7ACCu + 280u ||
        phys >= 0xB7ACCu + 10u * 608u + 280u) return 0;
    uint32_t actor = phys - 280u;
    if ((actor - 0xB7ACCu) % 608u) return 0;
    const uint8_t *ram = memory_get_ram_ptr();
    if (!ram) return 0;
    int16_t old = (int16_t)((uint16_t)ram[phys] | ((uint16_t)ram[phys + 1] << 8));
    if (old <= 0 || (int16_t)value >= old) return 0;
    int bones = dc_actor_model_bones(ram, actor);
    if (!(creature_enabled && (bones == 22 || bones == 18 || bones == 7)) &&
        !(foreign_enabled && dc_fk_bank(pc))) return 0;
    if (!dc_recent_shot_victim((int)((actor - 0xB7ACCu) / 608u)) ||
        dc_call_active(cpu, DC_CALL_CREATURE_BLOOD)) return 0;
    DcCallFrame *frame = dc_call_begin(cpu, pc, DC_CALL_CREATURE_BLOOD);
    frame->values[0] = address; frame->values[1] = value;
    frame->values[2] = actor; frame->values[8] = instruction;
    return 1;
}

static void queue(CPUState *cpu, DcCallFrame *frame)
{
    cpu->gpr[4] = 0x80000000u | frame->values[2];
    cpu->gpr[31] = frame->continuation;
    psx_pgxp_forget_register(4); psx_pgxp_forget_register(31);
    cpu->pc = 0x8002E7CCu;
}

int dc_creature_blood_flush(CPUState *cpu, uint32_t next_pc)
{
    if (!dc_call_has_kind(DC_CALL_CREATURE_BLOOD)) return 0;
    DcCallFrame *frame = dc_call_active(cpu, DC_CALL_CREATURE_BLOOD);
    if (!frame || frame->values[7]) return 0;
    if (!next_pc || (next_pc & 3u) || cpu->gpr[29] != frame->stack)
        fail("creature blood: invalid instruction continuation");
    /* Capture after the enclosing branch and its delay slot have completed. */
    cpu->pc = next_pc;
    dc_guest_registers_capture(cpu, &frame->guest);
    frame->values[3] = dc_rng_seed_get();
    int count = dc_blood_mult();
    frame->values[4] = count > 0 ? (uint32_t)count : 1u;
    frame->values[6] = next_pc; frame->values[7] = 1;
    queue(cpu, frame);
    return 1;
}

int dc_creature_blood_pending(CPUState *cpu, uint32_t pc)
{
    if (!dc_call_has_kind(DC_CALL_CREATURE_BLOOD)) return 0;
    DcCallFrame *frame = dc_call_lookup(cpu, pc, DC_CALL_CREATURE_BLOOD);
    return frame && frame->state == DC_CALL_ACTIVE && frame->values[7] == 1;
}

int dc_creature_blood_resume(CPUState *cpu, uint32_t pc)
{
    if (!dc_call_has_kind(DC_CALL_CREATURE_BLOOD)) return 0;
    DcCallFrame *frame = dc_call_lookup(cpu, pc, DC_CALL_CREATURE_BLOOD);
    if (!frame || frame->state != DC_CALL_ACTIVE) return 0;
    if (frame->values[7] != 1) fail("creature blood: callback was not dispatched");
    ++frame->values[5];
    if (frame->values[5] < frame->values[4] && dc_blood_pool_room()) {
        queue(cpu, frame); return 1;
    }
    dc_rng_seed_set(frame->values[3]);
    dc_guest_registers_restore(cpu, &frame->guest);
    g_debug_last_store_pc = frame->continuation;
    cpu->write_half(frame->values[0], (uint16_t)frame->values[1]);
    psx_pgxp_store(cpu, frame->values[8], frame->values[0], frame->values[1]);
    dc_call_clear(frame);
    return 1;
}
