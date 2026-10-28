#include "dc_fk_policy.h"
#include "dc_pgxp.h"
#include "pgxp_hooks.h"
#include "pst_wire.h"
#include <stdlib.h>
#include <string.h>

extern uint8_t *memory_get_ram_ptr(void);
extern volatile int g_dc_standdown;
enum { FK_FIRST = 0x02E00000u, FK_END = 0x02F00000u, FK_BYTES = 0x20000u,
       FK_WINDOW = 0xE8000u, FK_WINDOW_END = 0x100000u, GSV_FIRST = 0xB7990u,
       GSV_WORDS = 4, FK_BANKS = 8, FK_WIRE_BYTES = 200 };
typedef struct DcFkState {
    uint32_t room;
    uint32_t bank[FK_BANKS][GSV_WORDS], native[GSV_WORDS];
    uint8_t bank_have[FK_BANKS][GSV_WORDS], native_have[GSV_WORDS];
} DcFkState;
static DcFkState state = {UINT32_MAX};
static int slots_enabled = -1;

static int enabled(void) {
    if (slots_enabled < 0) {
        const char *value = getenv("DC_FK_GSSLOTS");
        slots_enabled = !(value && value[0] == '0');
    }
    return slots_enabled;
}
uint32_t dc_fk_bank(uint32_t pc) {
    uint32_t phys = pc & 0x1FFFFFFFu;
    return phys >= FK_FIRST && phys < FK_END ? FK_FIRST + ((phys - FK_FIRST) & ~(FK_BYTES - 1u)) : 0;
}
uint32_t dc_fk_address(uint32_t pc, uint32_t address) {
    uint32_t bank = dc_fk_bank(pc), phys = address & 0x1FFFFFFFu;
    return bank && phys >= FK_WINDOW && phys < FK_WINDOW_END ? 0x80000000u | (bank + phys - FK_WINDOW) : address;
}
uint32_t dc_fk_pointer(uint32_t pc, uint32_t value) {
    uint32_t bank = dc_fk_bank(pc);
    return bank && value >= 0x800E8000u && value < 0x80100000u ? 0x80000000u | (bank + value - 0x800E8000u) : value;
}
void dc_fk_leave(CPUState *cpu, uint32_t pc, uint32_t target) {
    if (!dc_fk_bank(pc) || dc_fk_bank(target)) return;
    for (uint32_t reg = 2; reg <= 7; ++reg) {
        uint32_t value = dc_fk_pointer(pc, cpu->gpr[reg]);
        if (value != cpu->gpr[reg]) {
            cpu->gpr[reg] = value;
            psx_pgxp_forget_register(reg);
            dc_pgxp_forget_register(reg);
        }
    }
}
static int slot(uint32_t address) {
    uint32_t phys = address & 0x1FFFFFFFu;
    return phys >= GSV_FIRST && phys < GSV_FIRST + 4u * GSV_WORDS && !(phys & 3u) ? (int)((phys - GSV_FIRST) / 4u) : -1;
}
void dc_fk_gsv_reset(void) { memset(&state, 0, sizeof state); state.room = UINT32_MAX; }
void dc_fk_room_tick(int room) {
    if (state.room != (uint32_t)room) { dc_fk_gsv_reset(); state.room = (uint32_t)room; }
}
uint32_t dc_fk_load_word(uint32_t pc, uint32_t address, uint32_t value) {
    int index = slot(address);
    if (index >= 0 && enabled()) {
        uint32_t bank = dc_fk_bank(pc);
        if (bank) {
            unsigned i = (bank - FK_FIRST) / FK_BYTES;
            if (state.bank_have[i][index]) value = state.bank[i][index];
        } else if (dc_fk_bank(value) && state.native_have[index]) value = state.native[index];
    }
    if (g_dc_standdown && (address & 0x1FFFFFFFu) == 0xB7890u) value |= 2u;
    return value;
}
uint32_t dc_fk_store_word(uint32_t pc, uint32_t address, uint32_t value) {
    value = dc_fk_pointer(pc, value);
    int index = slot(address);
    if (index >= 0 && enabled()) {
        uint32_t bank = dc_fk_bank(pc);
        if (bank) {
            const uint8_t *ram = memory_get_ram_ptr(); uint32_t current;
            if (ram) {
                memcpy(&current, ram + GSV_FIRST + (unsigned)index * 4u, 4);
                if (!dc_fk_bank(current)) { state.native[index] = current; state.native_have[index] = 1; }
            }
            unsigned i = (bank - FK_FIRST) / FK_BYTES;
            state.bank[i][index] = value; state.bank_have[i][index] = 1;
        } else { state.native[index] = value; state.native_have[index] = 1; }
    }
    return value;
}
uint32_t dc_fk_special(uint32_t player, uint32_t offset, uint32_t current) {
    if (!enabled() || offset < 0x140u || offset >= 0x150u) return current;
    unsigned index = (offset - 0x140u) / 4u;
    const uint8_t *ram = memory_get_ram_ptr(); uint32_t pl = player & 0x1FFFFFu;
    if (!ram || pl < 0x10000u || pl + 344u > 0x200000u) return current;
    uint32_t partner, table;
    memcpy(&partner, ram + pl + 340u, 4); partner &= 0x1FFFFFu;
    if (partner < 0xB7ACCu || partner >= 0xB7ACCu + 10u * 608u || (partner - 0xB7ACCu) % 608u) return current;
    memcpy(&table, ram + 0xB56F0u, 4); table &= 0x1FFFFFu;
    unsigned kind = ram[partner + 39u]; uint32_t fn = 0;
    if (kind < 32u && table >= 0x10000u && table + 128u <= 0x200000u) memcpy(&fn, ram + table + kind * 4u, 4);
    uint32_t bank = dc_fk_bank(fn);
    if (bank) {
        unsigned i = (bank - FK_FIRST) / FK_BYTES;
        if (state.bank_have[i][index]) return state.bank[i][index];
    } else if (dc_fk_bank(current) && state.native_have[index]) return state.native[index];
    return current;
}
size_t dc_fk_snapshot_bytes(void) { return FK_WIRE_BYTES; }
int dc_fk_snapshot_write(uint8_t *data, size_t size) {
    if (!data || size != FK_WIRE_BYTES) return 0;
    PstW out; pst_w_init(&out, data, size);
    if (!pst_w_u32(&out, 0x53474B46u) || !pst_w_u32(&out, 2) || !pst_w_u32(&out, FK_BANKS) || !pst_w_u32(&out, GSV_WORDS) || !pst_w_u32(&out, state.room)) return 0;
    for (unsigned i = 0; i <= FK_BANKS; ++i) for (unsigned j = 0; j < GSV_WORDS; ++j) {
        uint32_t value = i == FK_BANKS ? state.native[j] : state.bank[i][j];
        uint8_t have = i == FK_BANKS ? state.native_have[j] : state.bank_have[i][j];
        if (!pst_w_u32(&out, value) || !pst_w_u8(&out, have)) return 0;
    }
    return out.written == size;
}
static int decode(const uint8_t *data, size_t size, DcFkState *decoded) {
    if (!data || size != FK_WIRE_BYTES) return 0;
    PstR in; uint32_t magic, version, banks, words;
    pst_r_init(&in, data, size);
    if (!pst_r_u32(&in, &magic) || magic != 0x53474B46u || !pst_r_u32(&in, &version) || version != 2 ||
        !pst_r_u32(&in, &banks) || banks != FK_BANKS || !pst_r_u32(&in, &words) || words != GSV_WORDS || !pst_r_u32(&in, &decoded->room)) return 0;
    for (unsigned i = 0; i <= FK_BANKS; ++i) for (unsigned j = 0; j < GSV_WORDS; ++j) {
        uint32_t value; uint8_t have;
        if (!pst_r_u32(&in, &value) || !pst_r_u8(&in, &have) || have > 1) return 0;
        if (i == FK_BANKS) { decoded->native[j] = value; decoded->native_have[j] = have; }
        else { decoded->bank[i][j] = value; decoded->bank_have[i][j] = have; }
    }
    return in.p == in.end;
}
int dc_fk_snapshot_validate(const uint8_t *data, size_t size) { DcFkState decoded; return decode(data, size, &decoded); }
int dc_fk_snapshot_read(const uint8_t *data, size_t size) {
    DcFkState decoded;
    if (!decode(data, size, &decoded)) return 0;
    state = decoded; return 1;
}
