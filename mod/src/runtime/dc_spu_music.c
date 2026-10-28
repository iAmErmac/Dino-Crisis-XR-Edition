#include "dc_spu_music.h"
#include "dc_mods.h"
#include <stddef.h>
#include <string.h>

extern uint8_t *memory_get_ram_ptr(void);

typedef struct {
    int id;
    const int16_t *pcm;
    uint32_t frames, position, sequence;
    int32_t loop;
    uint8_t playing;
} DcMusicSlot;

static DcMusicSlot slots[3] = {{-1}, {-1}, {-1}};
static uint32_t mute_mask;

static uint32_t ram_word(const uint8_t *ram, uint32_t address)
{
    return (uint32_t)ram[address] | ((uint32_t)ram[address + 1] << 8) |
        ((uint32_t)ram[address + 2] << 16) | ((uint32_t)ram[address + 3] << 24);
}

static void resolve(DcMusicSlot *slot, const uint8_t *ram)
{
    int id = slot->sequence >= 0x10000u && slot->sequence < 0x1FFFF0u ?
        dc_mods_song_find(ram + slot->sequence, 0x200000u - slot->sequence) : -1;
    uint32_t frames = 0;
    int32_t loop = -1;
    const int16_t *pcm = id >= 0 ? dc_mods_song(id, &frames, &loop) : NULL;
    if (pcm != slot->pcm) slot->position = 0;
    slot->id = pcm && frames ? id : -1;
    slot->pcm = pcm;
    slot->frames = frames;
    slot->loop = loop;
}

void dc_spu_music_reset(void)
{
    memset(slots, 0, sizeof slots);
    for (unsigned i = 0; i < 3; ++i) slots[i].id = -1;
    mute_mask = 0;
}

void dc_spu_music_poll(void)
{
    if (!dc_mods_song_count()) { dc_spu_music_reset(); return; }
    const uint8_t *ram = memory_get_ram_ptr();
    if (!ram) return;
    for (unsigned i = 0; i < 3; ++i) {
        DcMusicSlot *slot = &slots[i];
        uint8_t playing = ram[0xC5A70u + i * 0xECu + 0x20u] == 1;
        uint32_t sequence = ram_word(ram, 0xC56F8u + i * 4u) & 0x1FFFFFu;
        if (playing && (!slot->playing || sequence != slot->sequence)) {
            slot->sequence = sequence;
            slot->position = 0;
            resolve(slot, ram);
        }
        if (!playing) { slot->id = -1; slot->pcm = NULL; }
        slot->playing = playing;
    }
    for (unsigned voice = 0; voice < 16; ++voice) {
        uint32_t address = 0xC5708u + voice * 0x24u;
        unsigned owner = ram[address + 0x22u];
        if (ram[address + 0x1Au] && owner < 3 && slots[owner].id >= 0)
            mute_mask |= 1u << voice;
    }
}

void dc_spu_music_key_on(unsigned voice)
{
    if (voice >= 24) return;
    const uint8_t *ram = memory_get_ram_ptr();
    unsigned owner = voice < 16 && ram && dc_mods_song_count() ?
        ram[0xC5708u + voice * 0x24u + 0x22u] : 0xFFu;
    if (owner < 3 && slots[owner].id >= 0) mute_mask |= 1u << voice;
    else mute_mask &= ~(1u << voice);
}

int dc_spu_music_muted(unsigned voice)
{
    return voice < 24 && (mute_mask & (1u << voice)) != 0;
}

void dc_spu_music_mix(int32_t *left, int32_t *right)
{
    const uint8_t *ram = memory_get_ram_ptr();
    if (!ram) return;
    for (unsigned i = 0; i < 3; ++i) {
        DcMusicSlot *slot = &slots[i];
        if (slot->id < 0 || !slot->pcm || !slot->frames) continue;
        if (slot->position >= slot->frames) {
            if (slot->loop < 0 || (uint32_t)slot->loop >= slot->frames) continue;
            slot->position = (uint32_t)slot->loop;
        }
        int volume = (int8_t)ram[0xC5A70u + i * 0xECu + 0x24u];
        unsigned menu = ram[0xB7850u + 0xA2CDu];
        int gain = (volume < 0 ? 0 : volume) * (menu > 15 ? 15 : menu);
        int32_t l = slot->pcm[slot->position * 2u] * gain / 1905;
        int32_t r = slot->pcm[slot->position * 2u + 1u] * gain / 1905;
        ++slot->position;
        if (!ram[0xB7850u + 0xA2CCu]) l = r = (l + r) / 2;
        *left += l;
        *right += r;
    }
}

int dc_spu_music_pcm_busy(const int16_t *pcm)
{
    if (!pcm) return 0;
    for (unsigned i = 0; i < 3; ++i)
        if (slots[i].id >= 0 && slots[i].pcm == pcm) return 1;
    return 0;
}

void dc_spu_music_changed(void)
{
    const uint8_t *ram = memory_get_ram_ptr();
    if (!dc_mods_song_count()) { dc_spu_music_reset(); return; }
    if (!ram) return;
    for (unsigned i = 0; i < 3; ++i)
        if (slots[i].id >= 0) resolve(&slots[i], ram);
}
