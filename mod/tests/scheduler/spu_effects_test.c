#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dc_spu_effects.h"
#include "test_process.h"

enum { EFFECT_SOURCE_BYTES = 32, EFFECT_SOUND_CAPACITY = 4 };

static uint8_t effect_source[EFFECT_SOURCE_BYTES];
static int sound_count;
static int match_id;
static unsigned match_calls;
static const int16_t *sound_pcm[EFFECT_SOUND_CAPACITY];
static uint32_t sound_samples[EFFECT_SOUND_CAPACITY];
static double sound_ratio[EFFECT_SOUND_CAPACITY];
static int32_t sound_loop[EFFECT_SOUND_CAPACITY];

int dc_mods_snd_count(void)
{
    return sound_count;
}

int dc_mods_snd_match(const uint8_t *bytes, uint32_t size, uint32_t address)
{
    ++match_calls;
    if (!bytes || size < 16u || address > size - 16u) return -1;
    return bytes[address] == 0xA5u ? match_id : -1;
}

const int16_t *dc_mods_snd(int id, uint32_t *n, double *ratio, int32_t *loop)
{
    if (n) *n = 0;
    if (ratio) *ratio = 0.0;
    if (loop) *loop = -1;
    if (id < 0 || id >= sound_count || id >= EFFECT_SOUND_CAPACITY) return NULL;
    if (n) *n = sound_samples[id];
    if (ratio) *ratio = sound_ratio[id];
    if (loop) *loop = sound_loop[id];
    return sound_pcm[id];
}

static void configure_sound(int id, const int16_t *pcm, uint32_t samples,
    double ratio, int32_t loop)
{
    assert(id >= 0 && id < EFFECT_SOUND_CAPACITY);
    sound_pcm[id] = pcm;
    sound_samples[id] = samples;
    sound_ratio[id] = ratio;
    sound_loop[id] = loop;
}

static void fixture_reset(void)
{
    memset(effect_source, 0, sizeof effect_source);
    effect_source[0] = 0xA5u;
    sound_count = 1;
    match_id = 0;
    match_calls = 0;
    for (unsigned i = 0; i < EFFECT_SOUND_CAPACITY; ++i) sound_pcm[i] = NULL;
    memset(sound_samples, 0, sizeof sound_samples);
    memset(sound_ratio, 0, sizeof sound_ratio);
    for (unsigned i = 0; i < EFFECT_SOUND_CAPACITY; ++i)
        sound_loop[i] = -1;
    dc_spu_effects_reset();
}

static void key_on(unsigned voice)
{
    dc_spu_effect_key_on(voice, effect_source, sizeof effect_source, 0);
}

static void next_expect(unsigned voice, uint16_t pitch, int expected_raw,
    int expected_end)
{
    int16_t raw = 1234;
    int ended = 1234;
    assert(dc_spu_effect_next(voice, pitch, &raw, &ended) == 1);
    assert(raw == expected_raw);
    assert(ended == expected_end);
}

static void assert_rejected_metadata(const int16_t *pcm, uint32_t samples,
    double ratio)
{
    fixture_reset();
    configure_sound(0, pcm, samples, ratio, -1);
    key_on(0);
    assert(!dc_spu_effect_pcm_busy(pcm));
    int16_t raw = 1234;
    int ended = 1234;
    assert(dc_spu_effect_next(0, 0, &raw, &ended) == 0);
    assert(raw == 0 && ended == 0);
}

static void test_interpolation_pitch_and_bounds(void)
{
    static const int16_t fractional_pcm[] = {0, 1000, 2000, 3000};
    fixture_reset();
    configure_sound(0, fractional_pcm, 4, 0.5, -1);
    key_on(0);
    next_expect(0, 0, 0, 0);
    next_expect(0, 0, 500, 0);
    next_expect(0, 0, 1000, 0);

    configure_sound(0, fractional_pcm, 4, 1.0, -1);
    key_on(0);
    next_expect(0, 0x2000u, 0, 0);
    next_expect(0, 0, 2000, 0);

    key_on(0);
    next_expect(0, 0x4000u, 0, 0);
    next_expect(0, 0, 1000, 0);

    assert(dc_spu_effect_next(24, 0, NULL, NULL) == 0);
    dc_spu_effect_key_on(23, effect_source, sizeof effect_source, 0);
    assert(dc_spu_effect_next(23, 0, NULL, NULL) == 1);
    assert(dc_spu_effect_pcm_busy(fractional_pcm));
    dc_spu_effect_key_on(24, effect_source, sizeof effect_source, 0);
    dc_spu_effect_retire(24);
    assert(dc_spu_effect_pcm_busy(fractional_pcm));
    dc_spu_effect_retire(23);
    dc_spu_effect_retire(0);
    assert(!dc_spu_effect_pcm_busy(fractional_pcm));
}

static void test_loop_overshoot_and_last_to_loop(void)
{
    static const int16_t pcm[] = {0, 100, 200, 300};
    fixture_reset();
    configure_sound(0, pcm, 4, 1.75, 1);
    key_on(0);
    next_expect(0, 0, 0, 0);
    next_expect(0, 0, 175, 0);
    next_expect(0, 0, 200, 0);
    next_expect(0, 0, 225, 0);
    assert(dc_spu_effect_pcm_busy(pcm));
}

static void test_nonloop_end_once_and_silent_tail(void)
{
    static const int16_t pcm[] = {1000, 3000};
    fixture_reset();
    configure_sound(0, pcm, 2, 1.5, -1);
    key_on(0);
    next_expect(0, 0, 1000, 0);
    next_expect(0, 0, 1500, 1);
    next_expect(0, 0, 0, 0);
    next_expect(0, 0, 0, 0);
    assert(dc_spu_effect_pcm_busy(pcm));
    dc_spu_effect_retire(0);
    assert(!dc_spu_effect_pcm_busy(pcm));
}

static void test_no_match_and_invalid_metadata(void)
{
    static const int16_t pcm[] = {10, 20};
    fixture_reset();
    configure_sound(0, pcm, 2, 1.0, -1);
    match_id = -1;
    key_on(0);
    assert(match_calls == 1);
    assert(!dc_spu_effect_pcm_busy(pcm));

    fixture_reset();
    configure_sound(0, pcm, 2, 1.0, -1);
    key_on(0);
    unsigned calls = match_calls;
    dc_spu_effect_key_on(0, NULL, sizeof effect_source, 0);
    dc_spu_effect_key_on(0, effect_source, 0, 0);
    dc_spu_effect_key_on(0, effect_source, 15, 0);
    dc_spu_effect_key_on(0, effect_source, sizeof effect_source, 17);
    dc_spu_effect_key_on(0, effect_source, sizeof effect_source, 1);
    dc_spu_effect_key_on(0, effect_source, 24, 0);
    assert(match_calls == calls);
    assert(!dc_spu_effect_pcm_busy(pcm));

    assert_rejected_metadata(pcm, 2, 0.0);
    assert_rejected_metadata(pcm, 2, -1.0);
    assert_rejected_metadata(pcm, 2, NAN);
    assert_rejected_metadata(pcm, 2, INFINITY);
    assert_rejected_metadata(pcm, 2, -INFINITY);
    assert_rejected_metadata(NULL, 2, 1.0);
    assert_rejected_metadata(pcm, 0, 1.0);

    fixture_reset();
    configure_sound(0, pcm, 2, 1.0, 99);
    key_on(0);
    next_expect(0, 0, 10, 0);
    next_expect(0, 0, 20, 1);
    next_expect(0, 0, 0, 0);
}

static void test_busy_reset_and_rekeyon_lifetime(void)
{
    static const int16_t old_pcm[] = {100, 300};
    static const int16_t new_pcm[] = {900, 1100};
    fixture_reset();
    configure_sound(0, old_pcm, 2, 1.0, -1);
    key_on(0);
    assert(dc_spu_effect_pcm_busy(old_pcm));

    configure_sound(0, new_pcm, 2, 1.0, -1);
    assert(dc_spu_effect_pcm_busy(old_pcm));
    assert(!dc_spu_effect_pcm_busy(new_pcm));

    key_on(0);
    assert(!dc_spu_effect_pcm_busy(old_pcm));
    assert(dc_spu_effect_pcm_busy(new_pcm));
    next_expect(0, 0, 900, 0);

    dc_spu_effects_reset();
    assert(!dc_spu_effect_pcm_busy(old_pcm));
    assert(!dc_spu_effect_pcm_busy(new_pcm));

    configure_sound(0, old_pcm, 1, 1.0, -1);
    key_on(0);
    next_expect(0, 0, 100, 1);
    assert(dc_spu_effect_pcm_busy(old_pcm));
    dc_spu_effects_reset();
    assert(!dc_spu_effect_pcm_busy(old_pcm));
}

int main(void)
{
    test_process_init();
    test_interpolation_pitch_and_bounds();
    test_loop_overshoot_and_last_to_loop();
    test_nonloop_end_once_and_silent_tail();
    test_no_match_and_invalid_metadata();
    test_busy_reset_and_rekeyon_lifetime();
    puts("PASS: SPU effect matching, interpolation, looping, bounds and PCM lifetime");
    return 0;
}
