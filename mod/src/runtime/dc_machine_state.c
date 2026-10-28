#include "dc_machine_state.h"
#include "dc_kernel_snapshot.h"
#include "dc_memory_snapshot.h"
#include "dc_continuation_wire.h"
#include "dc_memcard_snapshot.h"
#include "dc_fk_policy.h"
#include "pst_wire.h"
#include "interrupts.h"
#include <stdlib.h>
#include <string.h>

typedef struct DcMachineParts {
    uint32_t kernel_size, memory_size, continuation_size, card_size, fk_size;
    const uint8_t *kernel, *memory, *continuation, *cards, *fk;
} DcMachineParts;
typedef struct DcMachineContinuations {
    DcCallState calls;
    DcKernelEventState events;
    DcMemcardSnapshot cards;
} DcMachineContinuations;

static int dc_machine_parts(const uint8_t *data, uint32_t size, DcMachineParts *parts)
{
    PstR in; uint32_t version;
    if (!data || size < 24) return 0;
    pst_r_init(&in, data, size);
    if (!pst_r_u32(&in, &version) || version != 3 ||
        !pst_r_u32(&in, &parts->kernel_size) || !pst_r_u32(&in, &parts->memory_size) ||
        !pst_r_u32(&in, &parts->continuation_size) || !pst_r_u32(&in, &parts->card_size) || !pst_r_u32(&in, &parts->fk_size) ||
        parts->fk_size != dc_fk_snapshot_bytes() ||
        parts->card_size != dc_memcard_snapshot_wire_bytes() ||
        (uint64_t)parts->kernel_size + parts->memory_size + parts->continuation_size + parts->card_size + parts->fk_size + 24u != size ||
        parts->kernel_size != dc_kernel_snapshot_bytes() ||
        !parts->memory_size || !parts->continuation_size) return 0;
    parts->kernel = data + 24;
    parts->memory = parts->kernel + parts->kernel_size;
    parts->continuation = parts->memory + parts->memory_size;
    parts->cards = parts->continuation + parts->continuation_size;
    parts->fk = parts->cards + parts->card_size;
    return 1;
}

int dc_machine_state_capture(uint8_t **data, uint32_t *size)
{
    if (!data || !size) return 0;
    size_t kernel = dc_kernel_snapshot_bytes(), memory = dc_memory_snapshot_bytes();
    uint32_t continuation = dc_continuation_wire_bytes();
    size_t cards = dc_memcard_snapshot_wire_bytes();
    size_t fk = dc_fk_snapshot_bytes();
    uint64_t total = 24ull + kernel + memory + continuation + cards + fk;
    if (!kernel || !memory || !continuation || total > UINT32_MAX) return 0;
    uint8_t *output = malloc((size_t)total);
    DcMachineContinuations *state = malloc(sizeof *state);
    if (!output || !state) { free(state); free(output); return 0; }
    dc_call_capture(&state->calls); dc_kernel_event_capture(&state->events);
    PstW out; pst_w_init(&out, output, 24);
    int ok = pst_w_u32(&out, 3) && pst_w_u32(&out, (uint32_t)kernel) &&
        pst_w_u32(&out, (uint32_t)memory) && pst_w_u32(&out, continuation) && pst_w_u32(&out, (uint32_t)cards) && pst_w_u32(&out, (uint32_t)fk) &&
        dc_kernel_snapshot_write(output + 24, kernel) &&
        dc_memory_snapshot_write(output + 24 + kernel, memory) &&
        dc_continuation_wire_write(output + 24 + kernel + memory, continuation, &state->calls, &state->events) &&
        dc_memcard_snapshot_capture(&state->cards) &&
        dc_memcard_snapshot_write(&state->cards, output + 24 + kernel + memory + continuation, cards) &&
        dc_fk_snapshot_write(output + 24 + kernel + memory + continuation + cards, fk);
    free(state);
    if (!ok) { free(output); return 0; }
    *data = output; *size = (uint32_t)total;
    return 1;
}

static int dc_machine_decode(const uint8_t *data, uint32_t size,
    DcMachineParts *parts, DcMachineContinuations *state)
{
    return dc_machine_parts(data, size, parts) &&
        dc_kernel_snapshot_validate(parts->kernel, parts->kernel_size) &&
        dc_memory_snapshot_validate(parts->memory, parts->memory_size) &&
        dc_continuation_wire_read(parts->continuation, parts->continuation_size, &state->calls, &state->events) &&
        dc_memcard_snapshot_read(parts->cards, parts->card_size, &state->cards) &&
        dc_fk_snapshot_validate(parts->fk, parts->fk_size);
}

int dc_machine_state_validate(const uint8_t *data, uint32_t size)
{
    DcMachineParts parts;
    DcMachineContinuations *state = malloc(sizeof *state);
    if (!state) return 0;
    int ok = dc_machine_decode(data, size, &parts, state);
    free(state); return ok;
}

int dc_machine_state_restore(const uint8_t *data, uint32_t size, CPUState *cpu)
{
    if (!cpu || !data || size < 24 || psx_get_in_exception()) return 0;
    uint8_t *owned = malloc(size);
    DcMachineContinuations *state = malloc(sizeof *state);
    if (!owned || !state) { free(owned); free(state); return 0; }
    memcpy(owned, data, size);
    DcMachineParts parts;
    int ok = dc_machine_decode(owned, size, &parts, state);
    if (ok) ok = dc_memory_snapshot_read(parts.memory, parts.memory_size);
    if (ok) ok = dc_kernel_snapshot_read(parts.kernel, parts.kernel_size, cpu);
    if (ok) ok = dc_call_restore(&state->calls) && dc_kernel_event_restore(&state->events);
    if (ok) {
        const char *rewind = getenv("DC_SS_CARD_REWIND");
        ok = dc_memcard_snapshot_restore(&state->cards,
            rewind && rewind[0] == '1' ? DC_MEMCARD_RESTORE_REWIND : DC_MEMCARD_RESTORE_KEEP_DISK, NULL);
    }
    if (ok) ok = dc_fk_snapshot_read(parts.fk, parts.fk_size);
    free(state); free(owned); return ok;
}
