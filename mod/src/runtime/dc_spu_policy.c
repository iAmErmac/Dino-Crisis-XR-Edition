#include "dc_spu_policy.h"
#include "pst_wire.h"
#include <stdlib.h>

extern void psx_fatal_halt(const char *reason);

enum { POLICY_WIRE_VERSION = 1, DC_SPU_CD_DELAY_MAX = 2048 };

static uint32_t guard_addresses[DC_SPU_POLICY_VOICES];
static uint32_t cd_delay_remaining;
static int guards_initialized;
static int guard_enabled = 1;
static int cd_lead_enabled = 1;
static uint32_t cd_lead_pad = 64;

static void ensure_guards(void)
{
    if (guards_initialized) return;
    for (unsigned i = 0; i < DC_SPU_POLICY_VOICES; ++i)
        guard_addresses[i] = UINT32_MAX;
    guards_initialized = 1;
}

static void read_configuration(void)
{
    const char *guard = getenv("DC_SPU_END_GUARD");
    const char *lead = getenv("DC_CD_LEAD");
    const char *pad = getenv("DC_CD_LEAD_PAD");
    long value = pad ? strtol(pad, NULL, 10) : 64;

    guard_enabled = !guard || *guard != '0';
    cd_lead_enabled = !lead || *lead != '0';
    if (value < 0) value = 0;
    if (value > DC_SPU_CD_DELAY_MAX) value = DC_SPU_CD_DELAY_MAX;
    cd_lead_pad = (uint32_t)value;
}

void dc_spu_policy_reset(void)
{
    ensure_guards();
    for (unsigned i = 0; i < DC_SPU_POLICY_VOICES; ++i)
        guard_addresses[i] = UINT32_MAX;
    read_configuration();
    cd_delay_remaining = 0;
}

void dc_spu_guard_key_on(unsigned voice, const uint8_t *native_ram,
    uint32_t address)
{
    ensure_guards();
    if (voice >= DC_SPU_POLICY_VOICES) return;
    guard_addresses[voice] = UINT32_MAX;
    if (!guard_enabled || !native_ram || address >= DC_SPU_POLICY_RAM_BYTES ||
        (address & 15u)) return;

    uint32_t candidate = address;
    for (uint32_t block = 0; block < DC_SPU_POLICY_RAM_BYTES / 16u; ++block) {
        if (native_ram[candidate + 1u] & 0x01u) {
            guard_addresses[voice] = candidate;
            return;
        }
        candidate = (candidate + 16u) & (DC_SPU_POLICY_RAM_BYTES - 1u);
    }
}

uint8_t dc_spu_guard_flags(unsigned voice, uint32_t address, uint8_t flags)
{
    ensure_guards();
    if (!guard_enabled || voice >= DC_SPU_POLICY_VOICES ||
        guard_addresses[voice] != address ||
        (flags & 0x01u)) return flags;
    guard_addresses[voice] = UINT32_MAX;
    return 0x01u;
}

void dc_spu_cd_delay_reset(void)
{
    cd_delay_remaining = 0;
}

void dc_spu_cd_delay_push(int was_empty)
{
    if (was_empty && cd_lead_enabled) cd_delay_remaining = cd_lead_pad;
}

int dc_spu_cd_delay_tick(int queued)
{
    if (!cd_lead_enabled || !queued || !cd_delay_remaining) return 0;
    --cd_delay_remaining;
    return 1;
}

void dc_spu_policy_snapshot_write(uint8_t *data)
{
    if (!data) { psx_fatal_halt("SPU: missing policy snapshot buffer"); abort(); }
    ensure_guards();
    PstW out;
    pst_w_init(&out, data, DC_SPU_POLICY_STATE_BYTES);
    pst_w_u32(&out, POLICY_WIRE_VERSION);
    for (unsigned i = 0; i < DC_SPU_POLICY_VOICES; ++i)
        pst_w_u32(&out, guard_addresses[i]);
    pst_w_u32(&out, cd_delay_remaining);
}

int dc_spu_policy_snapshot_validate(const uint8_t *data, uint32_t size)
{
    if (!data || size != DC_SPU_POLICY_STATE_BYTES) return 0;

    PstR in;
    uint32_t version;
    uint32_t addresses[DC_SPU_POLICY_VOICES];
    uint32_t delay;
    pst_r_init(&in, data, size);
    if (!pst_r_u32(&in, &version) || version != POLICY_WIRE_VERSION) return 0;
    for (unsigned i = 0; i < DC_SPU_POLICY_VOICES; ++i) {
        if (!pst_r_u32(&in, &addresses[i])) return 0;
        if (addresses[i] != UINT32_MAX &&
            ((addresses[i] & 15u) || addresses[i] >= DC_SPU_POLICY_RAM_BYTES))
            return 0;
    }
    if (!pst_r_u32(&in, &delay) || delay > DC_SPU_CD_DELAY_MAX) return 0;
    return 1;
}

int dc_spu_policy_snapshot_read(const uint8_t *data, uint32_t size)
{
    if (!dc_spu_policy_snapshot_validate(data, size)) return 0;

    PstR in;
    uint32_t version;
    uint32_t addresses[DC_SPU_POLICY_VOICES];
    uint32_t delay;
    pst_r_init(&in, data, size);
    pst_r_u32(&in, &version);
    for (unsigned i = 0; i < DC_SPU_POLICY_VOICES; ++i)
        pst_r_u32(&in, &addresses[i]);
    pst_r_u32(&in, &delay);

    for (unsigned i = 0; i < DC_SPU_POLICY_VOICES; ++i)
        guard_addresses[i] = addresses[i];
    cd_delay_remaining = delay;
    guards_initialized = 1;
    return 1;
}
