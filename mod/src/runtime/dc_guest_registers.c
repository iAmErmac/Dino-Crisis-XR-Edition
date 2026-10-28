#include "dc_guest_registers.h"
#include <stdlib.h>
#include <string.h>
extern void psx_fatal_halt(const char *reason);

void dc_guest_registers_capture(CPUState *cpu, DcGuestRegisters *state)
{
    memcpy(state->words, cpu->gpr, sizeof cpu->gpr);
    state->words[32] = cpu->hi; state->words[33] = cpu->lo; state->pc = cpu->pc;
    psx_pgxp_capture_registers(state->words, &state->generic);
    dc_pgxp_capture_registers(state->words, &state->address);
}

void dc_guest_registers_restore(CPUState *cpu, const DcGuestRegisters *state)
{
    if (!psx_pgxp_restore_registers(state->words, &state->generic)) {
        psx_fatal_halt("guest callback: invalid register precision"); abort();
    }
    dc_pgxp_restore_registers(state->words, &state->address);
    memcpy(cpu->gpr, state->words, sizeof cpu->gpr);
    cpu->hi = state->words[32]; cpu->lo = state->words[33]; cpu->pc = state->pc;
}

void dc_guest_registers_restore_mask(CPUState *cpu, const DcGuestRegisters *state, uint32_t mask)
{
    DcGuestRegisters current;
    dc_guest_registers_capture(cpu, &current);
    for (unsigned i = 1; i < 32; ++i) {
        if (!(mask & (1u << i))) continue;
        current.words[i] = state->words[i];
        memset(&current.generic.reg[i], 0, sizeof current.generic.reg[i]);
        current.address.value[i] = state->words[i];
        memset(&current.address.reg[i], 0, sizeof current.address.reg[i]);
        current.address.source[i] = 0;
        if (current.generic.generation == state->generic.generation)
            current.generic.reg[i] = state->generic.reg[i];
        if (current.address.generation == state->address.generation) {
            current.address.reg[i] = state->address.reg[i];
            current.address.source[i] = state->address.source[i];
        }
    }
    dc_guest_registers_restore(cpu, &current);
}
