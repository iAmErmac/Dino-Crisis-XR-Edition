#ifndef DC_KERNEL_EVENTS_H
#define DC_KERNEL_EVENTS_H
#include "dc_guest_registers.h"
#ifdef __cplusplus
extern "C" {
#endif

enum { DC_KERNEL_EVENT_VERSION = 2, DC_KERNEL_EVENT_CAPACITY = 32 };
typedef struct DcKernelEventFrame {
    uint32_t owner, parent, stack, continuation, cls, spec, index, hit, waiting;
    uint32_t kind, class_index, result, arm_delay, arm_spec;
    DcGuestRegisters saved;
} DcKernelEventFrame;
typedef struct DcKernelEventState {
    uint32_t version;
    DcKernelEventFrame frames[DC_KERNEL_EVENT_CAPACITY];
} DcKernelEventState;
void dc_kernel_event_begin(CPUState *cpu, uint32_t cls, uint32_t spec);
void dc_kernel_event_defer_card(CPUState *cpu, uint32_t spec, uint32_t arm_delay, uint32_t arm_spec);
int dc_kernel_event_complete_native(CPUState *cpu);
int dc_kernel_event_pending(CPUState *cpu, uint32_t pc);
int dc_kernel_event_resume(CPUState *cpu, uint32_t pc);
void dc_kernel_event_discard_thread(uint32_t tcb);
void dc_kernel_event_reset(void);
void dc_kernel_event_capture(DcKernelEventState *state);
int dc_kernel_event_validate(const DcKernelEventState *state);
int dc_kernel_event_restore(const DcKernelEventState *state);
#ifdef __cplusplus
}
#endif
#endif
