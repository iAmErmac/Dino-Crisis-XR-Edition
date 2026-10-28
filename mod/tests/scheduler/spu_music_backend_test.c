#define main dc_spu_pages_backend_main
#include "spu_pages_backend_test.c"
#undef main
#include "dc_spu_music.h"

static uint8_t game_ram[0x200000];
static int song_count;
static const int16_t music_pcm[] = {12000, 6000, 8000, 2000, 4000, -2000};
uint8_t *memory_get_ram_ptr(void) { return game_ram; }
int dc_mods_song_count(void) { return song_count; }
int dc_mods_song_find(const uint8_t *seq, uint32_t maxlen)
{ assert(maxlen > 16); return seq[0] == 0xA5 ? 0 : -1; }
const int16_t *dc_mods_song(int id, uint32_t *frames, int32_t *loop)
{ assert(id == 0); *frames = 3; *loop = 1; return music_pcm; }

static void music_setup(void)
{
    cycle = 0; spu_init(); memset(game_ram, 0, sizeof game_ram); song_count = 1;
    game_ram[0xC5A90] = 1; game_ram[0xC5A94] = 127;
    game_ram[0xC56FA] = 1; game_ram[0x10000] = 0xA5;
    game_ram[0xB7850 + 0xA2CD] = 15; game_ram[0xB7850 + 0xA2CC] = 1;
    spu_write(0x1F801D80u, 0x3FFF); spu_write(0x1F801D82u, 0x3FFF);
    spu_write(0x1F801DAAu, 0x8000);
}

static void test_music_mix_paths(void)
{
    int16_t out[8];
    music_setup();
    spu_render_block(out, 4);
    assert(out[0] == 11999 && out[1] == 5999);
    assert(out[2] == 7999 && out[3] == 1999);
    assert(out[4] == 3999 && out[5] == -2000);
    assert(out[6] == 7999 && out[7] == 1999);
    assert(dc_spu_music_pcm_busy(music_pcm));

    music_setup();
    seed(spu_ram, 0x11); start(0, 0);
    spu_write(0x1F801C00u, 0x3FFF); spu_write(0x1F801C02u, 0x3FFF);
    voices[0].env_level = 0x7FFF; voices[0].adsr_phase = ADSR_SUSTAIN;
    voices[0].samples[0] = 4096; voices[0].sample_idx = 0;
    spu_write(0x1F801C04u, 0x1000);
    game_ram[0xC5722] = 1; game_ram[0xC572A] = 0;
    spu_render_block(out, 1);
    assert(dc_spu_music_muted(0));
    assert(out[0] == 11999 && out[1] == 5999);
    assert(voices[0].sample_idx == 1 && voices[0].active);

    music_setup(); spu_write(0x1F801DAAu, 0);
    spu_render_block(out, 1); assert(!out[0] && !out[1]);
    spu_write(0x1F801DAAu, 0x8000);
    spu_render_block(out, 1); assert(out[0] == 11999 && out[1] == 5999);
}

static void test_music_failed_restore(void)
{
    int16_t out[2];
    music_setup(); spu_render_block(out, 1);
    uint32_t size = spu_snapshot_bytes(); uint8_t *bad = malloc(size); assert(bad);
    spu_snapshot_write(bad);
    uint32_t bank_offset = SPU_REG_COUNT * 2u + 12u;
    bad[bank_offset] = SPU_SAMPLE_BANKS;
    assert(!spu_snapshot_read(bad, size));
    assert(dc_spu_music_pcm_busy(music_pcm));
    spu_render_block(out, 1); assert(out[0] == 7999 && out[1] == 1999);
    free(bad);
}

static void test_music_clock_and_reset(void)
{
    int16_t out[4];
    music_setup();
    cycle = 768u * 2u; spu_catch_up();
    assert(spu_output_pop(out, 2) == 2);
    assert(out[0] == 11999 && out[2] == 7999);
    uint32_t size = spu_snapshot_bytes(); uint8_t *saved = malloc(size); assert(saved);
    spu_snapshot_write(saved);
    assert(spu_snapshot_read(saved, size));
    assert(!dc_spu_music_pcm_busy(music_pcm));
    cycle += 768; spu_catch_up();
    assert(spu_output_pop(out, 1) == 1 && out[0] == 11999);
    spu_init(); assert(!dc_spu_music_pcm_busy(music_pcm));
    free(saved);
}

int main(void)
{
    dc_spu_pages_backend_main();
    test_music_mix_paths(); test_music_failed_restore(); test_music_clock_and_reset();
    puts("PASS: replacement music in both native mix paths and guest-clock output");
    return 0;
}
