#ifndef DC_GUEST_REGISTERS_H
#define DC_GUEST_REGISTERS_H
#include "cpu_state.h"
#include "dc_register_precision.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct DcGuestRegisters {
    uint32_t words[34], pc;
    PsxRegisterPrecision generic;
    DcAddressPrecision address;
} DcGuestRegisters;
void dc_guest_registers_capture(CPUState *cpu, DcGuestRegisters *state);
void dc_guest_registers_restore(CPUState *cpu, const DcGuestRegisters *state);
void dc_guest_registers_restore_mask(CPUState *cpu, const DcGuestRegisters *state, uint32_t mask);
uint32_t dc_grenade_take(void);
void dc_grenade_complete(uint32_t object);
#ifdef __cplusplus
}
#endif
#endif
