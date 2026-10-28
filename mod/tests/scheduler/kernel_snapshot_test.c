#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "dc_kernel.c"
#include "test_process.h"

#pragma push_macro("main")
#define main irq_fixture_main
#include "kernel_irq_test.c"
#undef main
#pragma pop_macro("main")

static void same_state(const uint8_t *expected, size_t size)
{
    uint8_t *actual = malloc(size); assert(actual);
    assert(dc_kernel_snapshot_write(actual, size));
    assert(!memcmp(actual, expected, size)); free(actual);
}

int main(void)
{
    test_process_init();
    CPUState cpu = {0}, other = {0};
    size_t size = dc_kernel_snapshot_bytes();
    uint8_t *wire = malloc(size), *bad = malloc(size);
    assert(wire && bad && size > 400);
    for (unsigned i = 0; i < IRQ_PRIO; ++i) s_irq_chain[i] = 0x80010000u + i * 16;
    s_entry_hook = 0x80020000u; s_exception_tcb = 0xA000E100u;
    s_card_late.armed = 1; s_card_late.vbl = 2; s_card_late.spec = 0x12345678u;
    s_pad_buf[0] = 0x80018000u; s_pad_len[0] = 34;
    s_pad_buf[1] = 0x80018100u; s_pad_len[1] = 8; s_pad_started = 1;
    s_ff.active = 1; s_ff.card = 1; s_ff.idx = CARD_DIR_N - 1;
    strcpy(s_ff.pat, "DINO*"); s_ff.entry = 0x8001F000u;
    for (unsigned i = 0; i < CARD_FD_N; ++i) {
        s_cfd[i].used = 1; s_cfd[i].card = i % 2; s_cfd[i].first = i;
        s_cfd[i].size = 8192; s_cfd[i].pos = 8192 + (int)i;
        strcpy(s_cfd[i].name, "DINO");
    }
    s_rand_seed = 0xABCDEF01u; s_heap_base = 0x80030000u; s_heap_size = 0x12345u;
    s_cpu = &other;
    assert(dc_kernel_snapshot_write(wire, size));
    assert(wire[0] == 1 && wire[1] == 0 && wire[4] == IRQ_PRIO && wire[8] == CARD_FD_N);
    assert(dc_kernel_snapshot_validate(wire, size));
    s_calls[0][0] = 99;
    memset(s_irq_chain, 0, sizeof s_irq_chain); s_entry_hook = 0;
    memset(&s_card_late, 0, sizeof s_card_late); memset(&s_ff, 0, sizeof s_ff);
    memset(s_cfd, 0, sizeof s_cfd); s_rand_seed = 0; s_pad_started = 0;
    assert(dc_kernel_snapshot_read(wire, size, &cpu));
    assert(s_cpu == &cpu && s_calls[0][0] == 99);
    assert(s_card_late.vbl == 2 && s_card_late.spec == 0x12345678u);
    assert(s_cfd[7].pos == 8199 && s_cfd[7].first == 7 && s_ff.idx == CARD_DIR_N - 1);
    assert(s_rand_seed == 0xABCDEF01u && s_heap_size == 0x12345u);
    same_state(wire, size);
    uint32_t sequence[3];
    for (unsigned i = 0; i < 3; ++i) {
        assert(hle_call_a0(&cpu, 0x2F)); sequence[i] = cpu.gpr[2];
    }
    assert(hle_file_seek(8, 100, 1) == 8299);
    assert(dc_kernel_snapshot_read(wire, size, &cpu));
    assert(hle_kernel_ready());
    assert(hle_file_seek(8, 100, 1) == 8299);
    for (unsigned i = 0; i < 3; ++i) {
        assert(hle_call_a0(&cpu, 0x2F)); assert(cpu.gpr[2] == sequence[i]);
    }
    assert(dc_kernel_snapshot_read(wire, size, &cpu));
    for (size_t n = 0; n < size; ++n) {
        assert(!dc_kernel_snapshot_validate(wire, n));
        assert(!dc_kernel_snapshot_read(wire, n, &other));
    }
    assert(!dc_kernel_snapshot_validate(wire, size + 1));
    assert(!dc_kernel_snapshot_read(wire, size, NULL));
    assert(!dc_kernel_snapshot_validate(NULL, size));
    memcpy(bad, wire, size); bad[0] = 2;
    assert(!dc_kernel_snapshot_read(bad, size, &other));
    memcpy(bad, wire, size); bad[8] = 9;
    assert(!dc_kernel_snapshot_read(bad, size, &other));
    /* Mutate decoded semantic fields and encode them without the public preflight. */
    DcKernelHostState invalid; PstW out;
    dc_kernel_host_capture(&invalid); invalid.file[7].first = CARD_DIR_N;
    pst_w_init(&out, bad, size); assert(dc_kernel_host_wire(&invalid, &out, NULL));
    assert(!dc_kernel_snapshot_read(bad, size, &other));
    dc_kernel_host_capture(&invalid); memset(invalid.pattern, 'X', sizeof invalid.pattern);
    pst_w_init(&out, bad, size); assert(dc_kernel_host_wire(&invalid, &out, NULL));
    assert(!dc_kernel_snapshot_validate(bad, size));
    same_state(wire, size); assert(s_cpu == &cpu);
    memset(bad, 0xA5, size); s_cfd[7].pos = -1;
    assert(!dc_kernel_snapshot_write(bad, size));
    for (size_t n = 0; n < size; ++n) assert(bad[n] == 0xA5);
    s_cfd[7].pos = 8199;
    same_state(wire, size);
    free(bad); free(wire); return 0;
}
