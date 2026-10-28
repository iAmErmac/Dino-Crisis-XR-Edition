#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dc_spu_policy.h"
#include "test_process.h"

static uint8_t native_ram[DC_SPU_POLICY_RAM_BYTES];

void psx_fatal_halt(const char *reason)
{
    (void)reason;
    abort();
}

static void set_env_value(const char *name, const char *value)
{
#ifdef _WIN32
    assert(_putenv_s(name, value) == 0);
#else
    assert(setenv(name, value, 1) == 0);
#endif
}

static void put_u32(uint8_t *data, uint32_t offset, uint32_t value)
{
    data[offset] = (uint8_t)value;
    data[offset + 1u] = (uint8_t)(value >> 8);
    data[offset + 2u] = (uint8_t)(value >> 16);
    data[offset + 3u] = (uint8_t)(value >> 24);
}

static void set_end(uint32_t address)
{
    assert(address < DC_SPU_POLICY_RAM_BYTES);
    assert((address & 15u) == 0);
    native_ram[address + 1u] = 0x01u;
}

static void policy_env_on(void)
{
    set_env_value("DC_CD_LEAD", "1");
    set_env_value("DC_CD_LEAD_PAD", "3");
    set_env_value("DC_SPU_END_GUARD", "1");
    dc_spu_policy_reset();
}

static void assert_snapshot_same(const uint8_t *expected)
{
    uint8_t actual[DC_SPU_POLICY_STATE_BYTES];
    dc_spu_policy_snapshot_write(actual);
    assert(memcmp(actual, expected, sizeof actual) == 0);
}

static void test_guards_and_bounds(void)
{
    const uint32_t start = 0x100u;
    const uint32_t first_end = 0x120u;
    const uint32_t wrap_start = 0x7FFF0u;
    const uint32_t wrap_end = 0x20u;

    memset(native_ram, 0, sizeof native_ram);
    policy_env_on();
    set_end(first_end);
    set_end(0x180u);
    dc_spu_guard_key_on(0, native_ram, start);
    assert(dc_spu_guard_flags(0, 0x110u, 0x52u) == 0x52u);
    assert(dc_spu_guard_flags(0, first_end, 0x03u) == 0x03u);
    assert(dc_spu_guard_flags(0, first_end, 0x00u) == 0x01u);
    assert(dc_spu_guard_flags(0, first_end, 0x02u) == 0x02u);

    dc_spu_guard_key_on(1, native_ram, start);
    assert(dc_spu_guard_flags(1, first_end, 0x02u) == 0x01u);
    assert(dc_spu_guard_flags(1, first_end, 0x02u) == 0x02u);

    memset(native_ram, 0, sizeof native_ram);
    dc_spu_guard_key_on(2, native_ram, 0x300u);
    assert(dc_spu_guard_flags(2, 0x300u, 0x02u) == 0x02u);

    set_end(wrap_end);
    dc_spu_guard_key_on(3, native_ram, wrap_start);
    assert(dc_spu_guard_flags(3, wrap_end, 0x00u) == 0x01u);

    memset(native_ram, 0, sizeof native_ram);
    dc_spu_guard_key_on(4, native_ram, 0x400u);
    assert(dc_spu_guard_flags(4, 0x400u, 0x02u) == 0x02u);

    dc_spu_guard_key_on(5, native_ram, DC_SPU_POLICY_RAM_BYTES);
    assert(dc_spu_guard_flags(5, 0x400u, 0x02u) == 0x02u);
    dc_spu_guard_key_on(5, native_ram, 3u);
    assert(dc_spu_guard_flags(5, 0x400u, 0x02u) == 0x02u);
    dc_spu_guard_key_on(5, NULL, 0x400u);
    assert(dc_spu_guard_flags(5, 0x400u, 0x02u) == 0x02u);

    dc_spu_guard_key_on(DC_SPU_POLICY_VOICES, native_ram, start);
    assert(dc_spu_guard_flags(DC_SPU_POLICY_VOICES, start, 0x02u) == 0x02u);

    memset(native_ram, 0, sizeof native_ram);
    set_end(0x500u);
    set_end(0x600u);
    dc_spu_guard_key_on(6, native_ram, 0x500u);
    dc_spu_guard_key_on(6, native_ram, 0x600u);
    assert(dc_spu_guard_flags(6, 0x500u, 0x00u) == 0x00u);
    assert(dc_spu_guard_flags(6, 0x600u, 0x00u) == 0x01u);
}

static void test_cd_delay(void)
{
    policy_env_on();
    dc_spu_cd_delay_reset();
    assert(dc_spu_cd_delay_tick(0) == 0);
    assert(dc_spu_cd_delay_tick(1) == 0);

    dc_spu_cd_delay_push(0);
    assert(dc_spu_cd_delay_tick(1) == 0);

    dc_spu_cd_delay_push(1);
    assert(dc_spu_cd_delay_tick(0) == 0);
    assert(dc_spu_cd_delay_tick(1) == 1);
    dc_spu_cd_delay_push(0);
    assert(dc_spu_cd_delay_tick(1) == 1);
    assert(dc_spu_cd_delay_tick(1) == 1);
    assert(dc_spu_cd_delay_tick(1) == 0);

    dc_spu_cd_delay_push(1);
    dc_spu_cd_delay_reset();
    assert(dc_spu_cd_delay_tick(1) == 0);
}

static void test_snapshot_validation_and_restore(void)
{
    const uint32_t saved_guard = 0x700u;
    uint8_t saved[DC_SPU_POLICY_STATE_BYTES];
    uint8_t before[DC_SPU_POLICY_STATE_BYTES];
    uint8_t candidate[DC_SPU_POLICY_STATE_BYTES];

    memset(native_ram, 0, sizeof native_ram);
    policy_env_on();
    set_end(saved_guard);
    dc_spu_guard_key_on(0, native_ram, saved_guard);
    dc_spu_cd_delay_push(1);
    dc_spu_policy_snapshot_write(saved);
    assert(dc_spu_policy_snapshot_validate(saved, sizeof saved));

    dc_spu_policy_reset();
    assert(dc_spu_policy_snapshot_read(saved, sizeof saved));
    assert(dc_spu_guard_flags(0, saved_guard, 0) == 1);
    assert(dc_spu_cd_delay_tick(1) == 1);
    assert(dc_spu_cd_delay_tick(1) == 1);
    assert(dc_spu_cd_delay_tick(1) == 1);
    assert(dc_spu_cd_delay_tick(1) == 0);

    dc_spu_policy_reset();
    set_end(0x740u);
    dc_spu_guard_key_on(1, native_ram, 0x740u);
    dc_spu_cd_delay_push(1);
    dc_spu_policy_snapshot_write(before);

    memcpy(candidate, before, sizeof candidate);
    put_u32(candidate, 0u, 2u);
    assert(!dc_spu_policy_snapshot_validate(candidate, sizeof candidate));
    assert(!dc_spu_policy_snapshot_read(candidate, sizeof candidate));
    assert_snapshot_same(before);

    memcpy(candidate, before, sizeof candidate);
    put_u32(candidate, 4u, 0x101u);
    assert(!dc_spu_policy_snapshot_validate(candidate, sizeof candidate));
    assert(!dc_spu_policy_snapshot_read(candidate, sizeof candidate));
    assert_snapshot_same(before);

    memcpy(candidate, before, sizeof candidate);
    put_u32(candidate, 4u, DC_SPU_POLICY_RAM_BYTES);
    assert(!dc_spu_policy_snapshot_validate(candidate, sizeof candidate));
    assert(!dc_spu_policy_snapshot_read(candidate, sizeof candidate));
    assert_snapshot_same(before);

    memcpy(candidate, before, sizeof candidate);
    put_u32(candidate, 100u, 2049u);
    assert(!dc_spu_policy_snapshot_validate(candidate, sizeof candidate));
    assert(!dc_spu_policy_snapshot_read(candidate, sizeof candidate));
    assert_snapshot_same(before);

    assert(!dc_spu_policy_snapshot_validate(saved, sizeof saved - 1u));
    assert(!dc_spu_policy_snapshot_read(saved, sizeof saved - 1u));
    assert(!dc_spu_policy_snapshot_validate(saved, sizeof saved + 1u));
    assert(!dc_spu_policy_snapshot_read(NULL, sizeof saved));

    dc_spu_policy_reset();
    assert(dc_spu_policy_snapshot_read(saved, sizeof saved));
    dc_spu_policy_snapshot_write(candidate);
    assert(memcmp(candidate, saved, sizeof candidate) == 0);
}

static void test_environment_policy(void)
{
    memset(native_ram, 0, sizeof native_ram);
    set_env_value("DC_CD_LEAD", "0");
    set_env_value("DC_CD_LEAD_PAD", "3000");
    set_env_value("DC_SPU_END_GUARD", "0");
    dc_spu_policy_reset();

    dc_spu_cd_delay_push(1);
    assert(dc_spu_cd_delay_tick(1) == 0);
    set_end(0x800u);
    dc_spu_guard_key_on(0, native_ram, 0x800u);
    assert(dc_spu_guard_flags(0, 0x800u, 0x02u) == 0x02u);

    set_env_value("DC_CD_LEAD", "1");
    set_env_value("DC_CD_LEAD_PAD", "3000");
    set_env_value("DC_SPU_END_GUARD", "1");
    dc_spu_policy_reset();
    dc_spu_cd_delay_push(1);
    for (unsigned i = 0; i < 2048u; ++i)
        assert(dc_spu_cd_delay_tick(1) == 1);
    assert(dc_spu_cd_delay_tick(1) == 0);

    set_env_value("DC_CD_LEAD_PAD", "-4");
    dc_spu_policy_reset();
    dc_spu_cd_delay_push(1);
    assert(dc_spu_cd_delay_tick(1) == 0);

    set_env_value("DC_CD_LEAD", "1");
    set_env_value("DC_CD_LEAD_PAD", "3");
    set_env_value("DC_SPU_END_GUARD", "1");
    dc_spu_policy_reset();
}

int main(void)
{
    test_process_init();
    test_guards_and_bounds();
    test_cd_delay();
    test_snapshot_validation_and_restore();
    test_environment_policy();
    puts("PASS: SPU guard, CD lead delay and policy snapshot behavior");
    return 0;
}
