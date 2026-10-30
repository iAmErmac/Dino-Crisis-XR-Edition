#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "pgxp_hooks.h"
#include "test_process.h"

#ifdef NDEBUG
#error Assertions must remain enabled for this regression test
#endif

struct CPUState {
    unsigned marker;
};

int g_pgxp_active;
int g_pgxp_addr_on;

static unsigned hook_calls[5];

static void assert_common_args(struct CPUState *cpu, uint32_t instruction)
{
    assert(cpu != NULL);
    assert(cpu->marker == 0xA55A);
    assert(instruction == 0x12345678);
}

void psx_pgxp_load(struct CPUState *cpu, uint32_t instruction,
                   uint32_t address, uint32_t value)
{
    assert_common_args(cpu, instruction);
    assert(address == 17);
    assert(value == 23);
    ++hook_calls[0];
}

void psx_pgxp_store(struct CPUState *cpu, uint32_t instruction,
                    uint32_t address, uint32_t value)
{
    assert_common_args(cpu, instruction);
    assert(address == 17);
    assert(value == 23);
    ++hook_calls[1];
}

void psx_pgxp_alu(struct CPUState *cpu, uint32_t instruction,
                  uint32_t result, uint32_t source1, uint32_t source2)
{
    assert_common_args(cpu, instruction);
    assert(result == 23);
    assert(source1 == 17);
    assert(source2 == 29);
    ++hook_calls[2];
}

void psx_pgxp_muldiv(struct CPUState *cpu, uint32_t instruction,
                     uint32_t hi, uint32_t lo,
                     uint32_t source1, uint32_t source2)
{
    assert_common_args(cpu, instruction);
    assert(hi == 31);
    assert(lo == 37);
    assert(source1 == 17);
    assert(source2 == 29);
    ++hook_calls[3];
}

void psx_pgxp_cop2(struct CPUState *cpu, uint32_t instruction,
                   uint32_t value, uint32_t address)
{
    assert_common_args(cpu, instruction);
    assert(value == 23);
    assert(address == 17);
    ++hook_calls[4];
}

static void emit_hook_calls(struct CPUState *cpu)
{
    /* An unbraced conditional catches macros that capture the else branch. */
    if (1)
        PGXP_LOAD(0x12345678, 17, 23);
    else
        assert(!"PGXP_LOAD changed the surrounding statement shape");

    PGXP_STORE(0x12345678, 17, 23);
    PGXP_ALU(0x12345678, 23, 17, 29);
    PGXP_MULDIV(0x12345678, 31, 37, 17, 29);
    PGXP_COP2(0x12345678, 23, 17);
}

static int expected_address_gated_hooks(void)
{
#if defined(PSX_PGXP) && PSX_PGXP
#if defined(DC_PGXP_ADDR) && !defined(PSX_OVERLAY_DLL_BUILD)
    return g_pgxp_addr_on || g_pgxp_active;
#else
    return 1;
#endif
#else
    return 0;
#endif
}

static int expected_muldiv_hook(void)
{
#if defined(PSX_PGXP) && PSX_PGXP
#if defined(DC_PGXP_ADDR) && !defined(PSX_OVERLAY_DLL_BUILD)
    return g_pgxp_active;
#else
    return 1;
#endif
#else
    return 0;
#endif
}

static void assert_counts(const unsigned expected[5])
{
    for (size_t hook = 0; hook < 5; ++hook) {
        assert(hook_calls[hook] == expected[hook]);
    }
}

int main(void)
{
    static const int transitions[][2] = {
        {0, 0}, {1, 0}, {0, 1}, {1, 1},
        {0, 0}, {0, 1}, {0, 0}, {1, 0},
    };
    struct CPUState state = {0xA55A};
    unsigned expected[5] = {0};

    test_process_init();

    for (size_t transition = 0;
         transition < sizeof(transitions) / sizeof(transitions[0]);
         ++transition) {
        g_pgxp_addr_on = transitions[transition][0];
        g_pgxp_active = transitions[transition][1];

        emit_hook_calls(&state);
        expected[0] += (unsigned)expected_address_gated_hooks();
        expected[1] += (unsigned)expected_address_gated_hooks();
        expected[2] += (unsigned)expected_address_gated_hooks();
        expected[3] += (unsigned)expected_muldiv_hook();
        expected[4] += (unsigned)expected_address_gated_hooks();
        assert_counts(expected);
    }

    puts("PASS: PGXP hook gates, arguments, statement safety, and re-enable transitions");
    return 0;
}
