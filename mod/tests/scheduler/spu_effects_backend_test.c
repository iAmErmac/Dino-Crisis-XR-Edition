#define main dc_spu_pages_backend_main
#include "spu_pages_backend_test.c"
#undef main
#include "dc_spu_effects.h"

static int effects_on, wanted_tag;
static int32_t effect_loop;
static double effect_ratio = 1.0;
static unsigned matches;
static const uint8_t *matched_source;
static const int16_t effect_pcm[] = {1000, 3000, 5000};
static uint8_t game_ram[0x200000];
static int music_on;
static const int16_t combined_music[] = {6000, 2000, 4000, 1000};
uint8_t *memory_get_ram_ptr(void) { return game_ram; }
int dc_mods_song_count(void) { return music_on; }
int dc_mods_song_find(const uint8_t *seq, uint32_t maxlen)
{ assert(maxlen > 16); return seq[0] == 0xA5 ? 0 : -1; }
const int16_t *dc_mods_song(int id, uint32_t *frames, int32_t *loop)
{ assert(id == 0); *frames = 2; *loop = 0; return combined_music; }
int dc_mods_snd_count(void) { return effects_on; }
int dc_mods_snd_match(const uint8_t *bytes, uint32_t size, uint32_t address)
{
    assert(bytes && address + 16u <= size);
    ++matches; matched_source = bytes;
    return bytes[address + 2u] == wanted_tag ? 0 : -1;
}
const int16_t *dc_mods_snd(int id, uint32_t *count, double *ratio, int32_t *loop)
{
    assert(id == 0); *count = 3; *ratio = effect_ratio; *loop = effect_loop;
    return effect_pcm;
}

static void effect_setup(void)
{
    music_on = 0; memset(game_ram, 0, sizeof game_ram);
    cycle = 0; spu_init(); seed(spu_ram, 0x11);
    effects_on = 1; wanted_tag = 0x11; matches = 0;
    effect_ratio = 1.0; effect_loop = -1;
    start(0, 0); spu_write(0x1F801C04u, 0x1000);
    voices[0].env_level = 0x7FFF; voices[0].adsr_phase = ADSR_SUSTAIN;
}

static void test_effect_envelope_and_end(void)
{
    effect_setup(); assert(spu_mods_pcm_busy(effect_pcm));
    assert(voice_next_sample(0) == 999);
    assert(voice_next_sample(0) == 2999);
    assert(!(endx_latch & 1u));
    assert(voice_next_sample(0) == 4999);
    assert((endx_latch & 1u) && voices[0].adsr_phase == ADSR_RELEASE);
    assert(voice_next_sample(0) == 0);
    voices[0].env_level = 0;
    assert(voice_next_sample(0) == 0);
    assert(!voices[0].active && !spu_mods_pcm_busy(effect_pcm));
}

static void test_effect_source_identity(void)
{
    effect_setup(); seed(spu_xpage(7), 0x33); wanted_tag = 0x33;
    start(0, 7); assert(matched_source == spu_xpage(7));
    assert(spu_mods_pcm_busy(effect_pcm));
    assert(voices[0].cur_addr == 0x100);
    uint8_t bank[0x120] = {0}; seed(bank, 0x22);
    assert(spu_register_sample_bank(1, bank, sizeof bank));
    wanted_tag = 0x22; assert(spu_bind_next_voice_bank(0, 1));
    unsigned matched_before_bank = matches;
    start(0, 7); assert(matches == matched_before_bank);
    assert(!spu_mods_pcm_busy(effect_pcm));
    decode_block(&voices[0]); assert(voices[0].samples[0] == 8192);

    wanted_tag = 0x11; start(0, 0);
    unsigned before = matches;
    spu_mods_changed(); start(0, 0);
    assert(matches == before + 1 && spu_mods_pcm_busy(effect_pcm));
    spu_ram[0x102] = 0x22; start(0, 0);
    assert(!spu_mods_pcm_busy(effect_pcm));
    assert(dc_spu_effect_next(24, 0x1000, NULL, NULL) == 0);
}

static void test_effect_snapshot_and_clock(void)
{
    effect_setup();
    spu_write(0x1F801D80u, 0x3FFF); spu_write(0x1F801D82u, 0x3FFF);
    spu_write(0x1F801C00u, 0x3FFF); spu_write(0x1F801C02u, 0x3FFF);
    spu_write(0x1F801DAAu, 0x8000);
    cycle = 768; spu_catch_up(); int16_t out[2];
    assert(spu_output_pop(out, 1) == 1 && out[0] == 997 && out[1] == 997);
    uint32_t size = spu_snapshot_bytes(); uint8_t *saved = malloc(size), *bad = malloc(size);
    assert(saved && bad); spu_snapshot_write(saved); memcpy(bad, saved, size);
    snapshot_put_u32(bad + native_voice_field(23, 16), SPU_SAMPLE_BANKS);
    assert(!spu_snapshot_read(bad, size) && spu_mods_pcm_busy(effect_pcm));
    assert(voice_next_sample(0) == 2999);
    assert(spu_snapshot_read(saved, size));
    assert(!spu_mods_pcm_busy(effect_pcm) && voices[0].active);
    start(0, 0); assert(spu_mods_pcm_busy(effect_pcm));
    spu_init(); assert(!spu_mods_pcm_busy(effect_pcm));
    free(saved); free(bad);
}

static void test_native_voice_without_replacement(void)
{
    cycle = 0; spu_init(); effects_on = 0; seed(spu_ram, 0x11);
    start(0, 0); voices[0].env_level = 0x7FFF; voices[0].adsr_phase = ADSR_SUSTAIN;
    voice_next_sample(0);
    uint32_t address = voices[0].cur_addr;
    assert(voices[0].sample_idx == 0 && voices[0].phase == 0);
    voice_next_sample(0);
    assert(voices[0].cur_addr == address && voices[0].sample_idx == 0);
    assert(!spu_mods_pcm_busy(effect_pcm));
    voices[0].sample_idx = SPU_BLOCK_SAMPLES;
    voices[0].flags = 1; voices[0].env_level = 0x7FFF;
    assert(voice_next_sample(0) == 0);
    assert(!voices[0].env_level && voices[0].adsr_phase == ADSR_RELEASE);
}

static void test_combined_replacement_sources(void)
{
    effect_setup(); music_on = 1;
    game_ram[0xC5A90] = 1; game_ram[0xC5A94] = 127;
    game_ram[0xC56FA] = 1; game_ram[0x10000] = 0xA5;
    game_ram[0xC5722] = 1; game_ram[0xC572A] = 0;
    game_ram[0xB7850 + 0xA2CD] = 15; game_ram[0xB7850 + 0xA2CC] = 1;
    spu_write(0x1F801D80u, 0x3FFF); spu_write(0x1F801D82u, 0x3FFF);
    spu_write(0x1F801C00u, 0x3FFF); spu_write(0x1F801C02u, 0x3FFF);
    spu_write(0x1F801DAAu, 0x8000);
    int16_t out[2]; spu_render_block(out, 1);
    assert(out[0] == 5999 && out[1] == 1999);
    assert(spu_mods_pcm_busy(effect_pcm) && spu_mods_pcm_busy(combined_music));
    assert(voice_next_sample(0) == 2999);
    spu_mods_changed();
    spu_render_block(out, 1);
    assert(out[0] == 3999 && out[1] == 999);
    spu_init();
    assert(!spu_mods_pcm_busy(effect_pcm) && !spu_mods_pcm_busy(combined_music));
}

int main(void)
{
    dc_spu_pages_backend_main();
    test_effect_envelope_and_end(); test_effect_source_identity(); test_effect_snapshot_and_clock(); test_native_voice_without_replacement(); test_combined_replacement_sources();
    puts("PASS: WAV source binding, actual ADSR/ENDX and guest-clock mixing");
    return 0;
}
