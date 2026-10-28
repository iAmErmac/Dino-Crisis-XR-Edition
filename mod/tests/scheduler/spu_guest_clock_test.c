#include "test_process.h"
#define main spu_guest_clock_contract_main
#include "test_spu_guest_clock.c"
#undef main
#include <stdlib.h>
void psx_fatal_halt(const char *reason) { (void)reason; abort(); }

static int reentrant_gate_calls;

static int reentrant_suppressed(void)
{
    if (reentrant_gate_calls++ == 0)
        spu_catch_up();
    return 1;
}

static void test_catch_up_reentrancy(void)
{
    stage_voice0();
    reentrant_gate_calls = 0;
    spu_set_output_gate(reentrant_suppressed);
    g_cycle = 768u * 4u;
    spu_catch_up();
    spu_set_output_gate(NULL);
    CHECK(reentrant_gate_calls == 1,
          "reentrant SPU catch-up is bounded by the existing guard");
    CHECK(s_clock == g_cycle && spu_output_available() == 0u,
          "reentrant catch-up preserves the guest sample bound and suppressed output");
}

int main(void)
{
    test_process_init();
    test_catch_up_reentrancy();
    return spu_guest_clock_contract_main();
}
