#include "dc_background.h"
#include "dc_call_state.h"
#include "pgxp_hooks.h"

#include <stdlib.h>

extern uint8_t *memory_get_ram_ptr(void);
extern int dc_nots_active(void);
extern int dc_scripted_now(void);
extern int dc_scene_flag_on(void);

enum { BACKGROUND_ARMED = 1, BACKGROUND_RUNNING = 2,
       BACKGROUND_HANDLER = 0x8002731Cu, BACKGROUND_RETURN = 0x8008F6F4u };

static int enabled(void)
{
    static int value = -1;
    if (value < 0) {
        const char *setting = getenv("DC_BG_SAMETICK");
        value = !(setting && setting[0] == '0');
    }
    return value;
}

static int background_offset(uint32_t pointer, uint32_t *offset)
{
    if ((pointer >> 24) != 0x80u ||
        pointer - 0x80000000u > 0x00200000u - 116u) return 0;
    *offset = pointer - 0x80000000u;
    return 1;
}

int dc_background_pending(CPUState *cpu, uint32_t pc)
{
    if (pc != BACKGROUND_RETURN || !dc_call_has_kind(DC_CALL_BACKGROUND)) return 0;
    DcCallFrame *frame = dc_call_lookup(cpu, pc, DC_CALL_BACKGROUND);
    return frame && frame->state == DC_CALL_ACTIVE &&
           frame->values[0] == BACKGROUND_RUNNING;
}

void dc_background_resume(CPUState *cpu, uint32_t pc)
{
    if (!dc_background_pending(cpu, pc)) return;
    DcCallFrame *frame = dc_call_resume(cpu, pc, DC_CALL_BACKGROUND);
    dc_guest_registers_restore(cpu, &frame->guest);
    dc_call_clear(frame);
}

int dc_background_enter(CPUState *cpu, uint32_t pc)
{
    uint32_t offset;
    uint8_t *ram;
    DcCallFrame *frame;

    if ((pc != BACKGROUND_HANDLER && pc != BACKGROUND_RETURN) || !enabled()) return 0;
    frame = dc_call_has_kind(DC_CALL_BACKGROUND) ?
        dc_call_active(cpu, DC_CALL_BACKGROUND) : NULL;
    if (pc == BACKGROUND_HANDLER) {
        /* A service call must not arm another service for its own return. */
        if (frame && frame->values[0] == BACKGROUND_RUNNING) return 0;
        if (frame) dc_call_clear(frame);
        if (cpu->gpr[31] != 0x800272B8u ||
            !background_offset(cpu->gpr[4], &offset)) return 0;
        ram = memory_get_ram_ptr();
        if (!ram || (ram[offset + 114u] & 1u)) return 0;
        frame = dc_call_begin(cpu, BACKGROUND_RETURN, DC_CALL_BACKGROUND);
        frame->values[0] = BACKGROUND_ARMED;
        return 0;
    }

    if (!frame || frame->values[0] != BACKGROUND_ARMED ||
        frame->stack != cpu->gpr[29] || cpu->gpr[31] != 0x8002730Cu) return 0;
    /* Consume the arm even when a scripted camera owns this transition. */
    ram = memory_get_ram_ptr();
    if (!background_offset(cpu->gpr[5], &offset) || !ram ||
        !(ram[offset + 114u] & 1u) || ram[0x0B92C8u] == 4 ||
        !dc_nots_active() || dc_scripted_now() || dc_scene_flag_on()) {
        dc_call_clear(frame);
        return 0;
    }

    cpu->pc = pc;
    dc_guest_registers_capture(cpu, &frame->guest);
    frame->values[0] = BACKGROUND_RUNNING;
    cpu->gpr[4] = cpu->gpr[5];
    cpu->gpr[31] = BACKGROUND_RETURN;
    psx_pgxp_forget_register(4);
    psx_pgxp_forget_register(31);
    cpu->pc = BACKGROUND_HANDLER;
    return 1;
}
