#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test_process.h"
#include "dc_spu_music.h"

static uint8_t game_ram[0x200000u];
static int song_count;
static unsigned find_calls;

static const int16_t song0_pcm[] = {
    1000, -1000, 2000, -2000, 3000, -3000, 4000, -4000
};
static const int16_t song1_pcm[] = {500, 1500, 700, 1700};
static const int16_t song2_pcm[] = {-300, 900, -500, 1100};
static const int16_t reload_pcm[] = {9000, -9000, 8000, -8000};
static const int16_t *song_pcm[3];
static uint32_t song_frames[3];
static int32_t song_loops[3];

uint8_t *memory_get_ram_ptr(void) { return game_ram; }

int dc_mods_song_count(void) { return song_count; }

int dc_mods_song_find(const uint8_t *seq, uint32_t maxlen)
{
    ++find_calls;
    if (!seq || !maxlen) return -1;
    if (seq[0] == 0xA0u) return 0;
    if (seq[0] == 0xB0u) return 1;
    if (seq[0] == 0xC0u) return 2;
    return -1;
}

const int16_t *dc_mods_song(int id, uint32_t *frames, int32_t *loop)
{
    if (frames) *frames = 0;
    if (loop) *loop = -1;
    if (id < 0 || id >= 3 || id >= song_count) return NULL;
    if (frames) *frames = song_frames[id];
    if (loop) *loop = song_loops[id];
    return song_pcm[id];
}

static void put_u32(uint32_t address, uint32_t value)
{
    game_ram[address] = (uint8_t)value;
    game_ram[address + 1u] = (uint8_t)(value >> 8);
    game_ram[address + 2u] = (uint8_t)(value >> 16);
    game_ram[address + 3u] = (uint8_t)(value >> 24);
}

static void put_sequence(uint32_t address, uint8_t tag)
{
    assert(address < sizeof game_ram);
    game_ram[address] = tag;
}

static void set_slot(unsigned slot, int playing, uint32_t sequence,
                     uint8_t tag, int volume)
{
    const uint32_t base = 0xC5A70u + slot * 0xECu;
    assert(slot < 3);
    put_u32(0xC56F8u + slot * 4u, sequence);
    if (sequence < sizeof game_ram) put_sequence(sequence, tag);
    game_ram[base + 0x20u] = (uint8_t)(playing ? 1 : 0);
    game_ram[base + 0x24u] = (uint8_t)volume;
}

static void set_voice_owner(unsigned voice, int active, unsigned owner)
{
    const uint32_t address = 0xC5708u + voice * 0x24u;
    assert(voice < 16 && owner < 256);
    game_ram[address + 0x1Au] = (uint8_t)(active ? 1 : 0);
    game_ram[address + 0x22u] = (uint8_t)owner;
}

static void mix_once(int32_t *left, int32_t *right)
{
    *left = 0;
    *right = 0;
    dc_spu_music_mix(left, right);
}

static void music_setup(void)
{
    memset(game_ram, 0, sizeof game_ram);
    song_count = 3;
    find_calls = 0;
    song_pcm[0] = song0_pcm;
    song_pcm[1] = song1_pcm;
    song_pcm[2] = song2_pcm;
    song_frames[0] = 4;
    song_frames[1] = 2;
    song_frames[2] = 2;
    song_loops[0] = 1;
    song_loops[1] = -1;
    song_loops[2] = 0;
    game_ram[0xB7850u + 0xA2CDu] = 20;
    game_ram[0xB7850u + 0xA2CCu] = 1;
    dc_spu_music_reset();
}

static void test_three_slots_and_mix(void)
{
    int32_t left, right;
    music_setup();
    set_slot(0, 1, 0x10000u, 0xA0u, 127);
    set_slot(1, 1, 0x10010u, 0xB0u, -1);
    set_slot(2, 1, 0x10020u, 0xC0u, 127);
    dc_spu_music_poll();
    assert(dc_spu_music_pcm_busy(song0_pcm));
    assert(dc_spu_music_pcm_busy(song1_pcm));
    assert(dc_spu_music_pcm_busy(song2_pcm));

    mix_once(&left, &right);
    assert(left == 700 && right == -100);

    game_ram[0xB7850u + 0xA2CCu] = 0;
    dc_spu_music_reset();
    set_slot(0, 1, 0x10000u, 0xA0u, 127);
    set_slot(1, 1, 0x10010u, 0xB0u, -1);
    set_slot(2, 1, 0x10020u, 0xC0u, 127);
    dc_spu_music_poll();
    mix_once(&left, &right);
    assert(left == 300 && right == 300);
}

static void test_sequence_start_stop_restart(void)
{
    int32_t left, right;
    music_setup();
    set_slot(0, 1, 0x10000u, 0xA0u, 127);
    dc_spu_music_poll();
    mix_once(&left, &right);
    assert(left == 1000 && right == -1000);

    set_slot(0, 0, 0x10000u, 0xA0u, 127);
    dc_spu_music_poll();
    assert(!dc_spu_music_pcm_busy(song0_pcm));
    mix_once(&left, &right);
    assert(left == 0 && right == 0);

    set_slot(0, 1, 0x10000u, 0xA0u, 127);
    dc_spu_music_poll();
    mix_once(&left, &right);
    assert(left == 1000 && right == -1000);

    set_slot(0, 1, 0x10010u, 0xB0u, 127);
    dc_spu_music_poll();
    mix_once(&left, &right);
    assert(left == 500 && right == 1500);
}

static void test_sequence_mismatch_and_bounds(void)
{
    int32_t left, right;
    music_setup();
    set_slot(0, 1, 0x10000u, 0xEEu, 127);
    dc_spu_music_poll();
    assert(find_calls == 1 && !dc_spu_music_pcm_busy(song0_pcm));
    mix_once(&left, &right);
    assert(left == 0 && right == 0);

    set_slot(0, 1, 0x0FFFFu, 0xA0u, 127);
    unsigned calls = find_calls;
    dc_spu_music_poll();
    assert(find_calls == calls && !dc_spu_music_pcm_busy(song0_pcm));

    set_slot(0, 1, 0x1FFFF0u, 0xA0u, 127);
    calls = find_calls;
    dc_spu_music_poll();
    assert(find_calls == calls && !dc_spu_music_pcm_busy(song0_pcm));
}

static void test_loop_and_nonloop_end(void)
{
    int32_t left, right;
    music_setup();
    set_slot(0, 1, 0x10000u, 0xA0u, 127);
    dc_spu_music_poll();
    mix_once(&left, &right); assert(left == 1000 && right == -1000);
    mix_once(&left, &right); assert(left == 2000 && right == -2000);
    mix_once(&left, &right); assert(left == 3000 && right == -3000);
    mix_once(&left, &right); assert(left == 4000 && right == -4000);
    mix_once(&left, &right); assert(left == 2000 && right == -2000);

    set_slot(0, 1, 0x10010u, 0xB0u, 127);
    dc_spu_music_poll();
    mix_once(&left, &right); assert(left == 500 && right == 1500);
    mix_once(&left, &right); assert(left == 700 && right == 1700);
    mix_once(&left, &right); assert(left == 0 && right == 0);
}

static void test_hardware_mute_and_owner_reassignment(void)
{
    music_setup();
    set_slot(0, 1, 0x10000u, 0xA0u, 127);
    set_slot(1, 1, 0x10010u, 0xB0u, 127);
    set_slot(2, 1, 0x10020u, 0xC0u, 127);
    for (unsigned voice = 0; voice < 16; ++voice)
        set_voice_owner(voice, 1, voice % 3u);
    dc_spu_music_poll();
    for (unsigned voice = 0; voice < 16; ++voice)
        assert(dc_spu_music_muted(voice));
    for (unsigned voice = 16; voice < 48; ++voice) {
        dc_spu_music_key_on(voice);
        assert(!dc_spu_music_muted(voice));
    }

    dc_spu_music_key_on(UINT32_MAX);
    assert(!dc_spu_music_muted(UINT32_MAX));
    set_voice_owner(0, 1, 3);
    dc_spu_music_key_on(0);
    assert(!dc_spu_music_muted(0));
    set_voice_owner(0, 1, 2);
    dc_spu_music_key_on(0);
    assert(dc_spu_music_muted(0));
}

static void test_reload_and_lifetime(void)
{
    int32_t left, right;
    music_setup();
    set_slot(0, 1, 0x10000u, 0xA0u, 127);
    dc_spu_music_poll();
    mix_once(&left, &right);
    assert(left == 1000 && right == -1000);
    assert(dc_spu_music_pcm_busy(song0_pcm));

    dc_spu_music_changed();
    mix_once(&left, &right);
    assert(left == 2000 && right == -2000);

    song_pcm[0] = reload_pcm;
    song_frames[0] = 2;
    song_loops[0] = -1;
    dc_spu_music_changed();
    mix_once(&left, &right);
    assert(left == 9000 && right == -9000);
    assert(dc_spu_music_pcm_busy(song0_pcm) == 0);
    assert(dc_spu_music_pcm_busy(reload_pcm));

    set_slot(0, 0, 0x10000u, 0xA0u, 127);
    dc_spu_music_poll();
    assert(!dc_spu_music_pcm_busy(reload_pcm));
    song_count = 0;
    dc_spu_music_poll();
    assert(!dc_spu_music_pcm_busy(reload_pcm));
    dc_spu_music_reset();
    assert(!dc_spu_music_muted(0));
}

int main(void)
{
    test_process_init();
    test_three_slots_and_mix();
    test_sequence_start_stop_restart();
    test_sequence_mismatch_and_bounds();
    test_loop_and_nonloop_end();
    test_hardware_mute_and_owner_reassignment();
    test_reload_and_lifetime();
    puts("PASS: music slots, sequence lifecycle, mix rules, mute ownership and reload lifetime");
    return 0;
}
