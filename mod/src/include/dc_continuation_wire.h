#ifndef DC_CONTINUATION_WIRE_H
#define DC_CONTINUATION_WIRE_H
#include "dc_call_state.h"
#include "dc_kernel_events.h"
#ifdef __cplusplus
extern "C" {
#endif
uint32_t dc_continuation_wire_bytes(void);
int dc_continuation_wire_write(uint8_t *data, uint32_t size,
    const DcCallState *calls, const DcKernelEventState *events);
int dc_continuation_wire_read(const uint8_t *data, uint32_t size,
    DcCallState *calls, DcKernelEventState *events);
#ifdef __cplusplus
}
#endif
#endif
