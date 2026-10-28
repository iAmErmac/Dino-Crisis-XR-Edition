#define main kernel_snapshot_fixture_main
#include "kernel_snapshot_test.c"
#undef main
#include "dc_machine_state.h"
#include "dc_fk_policy.h"
volatile int g_dc_standdown;
#include "dc_memcard_snapshot.h"
#include "dc_memory_policy.h"
#include "dc_continuation_wire.h"
#include "pst_wire.h"

static uint32_t wire_word(const uint8_t *p)
{
    PstR r; uint32_t value;
    pst_r_init(&r, p, 4); assert(pst_r_u32(&r, &value)); return value;
}

int main(void)
{
    assert(kernel_snapshot_fixture_main() == 0);
    CPUState cpu = {0};
    dc_call_reset(); dc_kernel_event_reset(); dc_fk_gsv_reset(); dc_fk_room_tick(19);
    assert(dc_fk_store_word(0x82E20000u, 0xB7990u, 0x800E8120u) == 0x82E20120u);
    DcCallState *calls = calloc(1, sizeof *calls), *after = calloc(1, sizeof *after);
    DcKernelEventState *events = calloc(1, sizeof *events);
    assert(calls && after && events);
    dc_call_capture(calls);
    DcCallFrame *frame = &calls->frames[63];
    frame->kind = DC_CALL_ANIMATION; frame->owner = 0xE29C;
    frame->stack = 0x801F0000; frame->continuation = 0x80018000;
    frame->state = DC_CALL_ACTIVE; frame->values[15] = 0x12345678;
    assert(dc_call_restore(calls));
    dc_kernel_event_capture(events);
    DcKernelEventFrame *event = &events->frames[31];
    event->owner = 0xE35C; event->stack = 0x801E0000;
    event->continuation = 0x80018100; event->waiting = 1; event->hit = 1;
    event->saved.words[29] = event->stack; event->saved.words[31] = event->continuation;
    assert(dc_kernel_event_restore(events));
    uint8_t *memory = dc_memory_extra_ptr(0x00400003u, 1); *memory = 0x5A;
    s_rand_seed = 0x11223344; s_cfd[7].pos = 8999;
    uint8_t *wire = NULL; uint32_t size = 0;
    assert(dc_machine_state_capture(&wire, &size) && wire && size > 100000);
    assert(wire_word(wire) == 3 && wire_word(wire + 4) == dc_kernel_snapshot_bytes());
    assert(dc_machine_state_validate(wire, size));
    s_rand_seed = 0; s_cfd[7].pos = 0; *memory = 0;
    dc_call_reset(); dc_kernel_event_reset(); dc_fk_gsv_reset(); dc_fk_room_tick(20);
    assert(dc_machine_state_restore(wire, size, &cpu));
    dc_fk_room_tick(19);
    assert(dc_fk_load_word(0x82E20000u, 0xB7990u, 0) == 0x82E20120u);
    assert(s_cpu == &cpu && s_rand_seed == 0x11223344 && s_cfd[7].pos == 8999 && *memory == 0x5A);
    dc_call_capture(after); assert(after->frames[63].values[15] == 0x12345678);
    dc_kernel_event_capture(events); assert(events->frames[31].owner == 0xE35C);
    uint8_t *again = NULL; uint32_t again_size = 0;
    assert(dc_machine_state_capture(&again, &again_size) && again_size == size);
    assert(!memcmp(wire, again, size));
    uint8_t *bad = malloc(size); assert(bad);
    uint32_t kernel = wire_word(wire + 4), mem = wire_word(wire + 8);
    const uint32_t corrupt[] = {0, 4, 16, 20, 24, 24 + kernel, 24 + kernel + mem,
        size - (uint32_t)dc_fk_snapshot_bytes() - DC_MEMCARD_SNAPSHOT_WIRE_BYTES,
        size - (uint32_t)dc_fk_snapshot_bytes(), size - 1};
    for (unsigned i = 0; i < sizeof corrupt / sizeof corrupt[0]; ++i) {
        memcpy(bad, wire, size); bad[corrupt[i]] ^= 0x7F;
        assert(!dc_machine_state_validate(bad, size));
        assert(!dc_machine_state_restore(bad, size, &cpu));
        assert(dc_fk_load_word(0x82E20000u, 0xB7990u, 0) == 0x82E20120u);
        assert(s_rand_seed == 0x11223344 && *memory == 0x5A && s_cfd[7].pos == 8999);
    }
    assert(!dc_machine_state_restore(wire, size - 1, &cpu));
    assert(!dc_machine_state_restore(wire, size, NULL));
    assert(!dc_machine_state_validate(NULL, size));
    assert(!dc_machine_state_validate(wire, size + 1));
    assert(!dc_machine_state_capture(NULL, &size));
    uint8_t *alias = dc_memory_extra_ptr(0x00420000u, size);
    memcpy(alias, wire, size);
    assert(dc_machine_state_restore(alias, size, &cpu));
    assert(*alias == 0 && *memory == 0x5A && s_cpu == &cpu);
    free(bad); free(again); free(wire); free(events); free(after); free(calls);
    return 0;
}
