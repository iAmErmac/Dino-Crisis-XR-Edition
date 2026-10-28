#include "dc_spu_effects.h"
#include "dc_mods.h"
#include <float.h>
#include <math.h>
#include <string.h>

typedef struct {
    const int16_t *pcm;
    uint32_t samples;
    int32_t loop;
    double ratio;
    double position;
    int done;
} DcSpuEffect;

static DcSpuEffect effects[DC_SPU_EFFECT_VOICE_COUNT];

static void clear_effect(DcSpuEffect *effect)
{
    memset(effect, 0, sizeof *effect);
    effect->loop = -1;
}

static int finite_positive(double value)
{
    return value > 0.0 && value <= DBL_MAX;
}

void dc_spu_effects_reset(void)
{
    for (unsigned i = 0; i < DC_SPU_EFFECT_VOICE_COUNT; ++i)
        clear_effect(&effects[i]);
}

void dc_spu_effect_key_on(unsigned voice, const uint8_t *bytes, uint32_t size,
    uint32_t address)
{
    if (voice >= DC_SPU_EFFECT_VOICE_COUNT) return;
    DcSpuEffect *effect = &effects[voice];
    clear_effect(effect);
    int count = dc_mods_snd_count();
    if (!bytes || size < 16u || (size & (size - 1u)) || (address & 15u) ||
        address > size - 16u || count <= 0) return;

    int id = dc_mods_snd_match(bytes, size, address);
    if (id < 0 || id >= count) return;

    uint32_t samples = 0;
    double ratio = 0.0;
    int32_t loop = -1;
    const int16_t *pcm = dc_mods_snd(id, &samples, &ratio, &loop);
    if (!pcm || !samples || !finite_positive(ratio)) return;

    effect->pcm = pcm;
    effect->samples = samples;
    effect->ratio = ratio;
    effect->loop = loop >= 0 && (uint32_t)loop < samples ? loop : -1;
}

int dc_spu_effect_next(unsigned voice, uint16_t pitch, int16_t *raw, int *ended)
{
    if (raw) *raw = 0;
    if (ended) *ended = 0;
    if (voice >= DC_SPU_EFFECT_VOICE_COUNT) return 0;

    DcSpuEffect *effect = &effects[voice];
    if (!effect->pcm || !effect->samples || !effect->ratio) return 0;
    if (effect->done) return 1;
    if (!(effect->position >= 0.0) || !isfinite(effect->position) ||
        effect->position >= (double)effect->samples) {
        effect->done = 1;
        return 1;
    }

    uint32_t index = (uint32_t)effect->position;
    double fraction = effect->position - (double)index;
    int32_t first = effect->pcm[index];
    int32_t second = 0;
    if (index + 1u < effect->samples)
        second = effect->pcm[index + 1u];
    else if (effect->loop >= 0)
        second = effect->pcm[(uint32_t)effect->loop];
    int32_t value = first + (int32_t)((second - first) * fraction);
    if (raw) *raw = (int16_t)value;

    uint16_t pitch_value = pitch & 0x3FFFu;
    if (!pitch_value) pitch_value = 0x1000u;
    double step = ((double)pitch_value / 4096.0) * effect->ratio;
    double next = effect->position + step;
    if (!(step > 0.0) || !isfinite(next)) {
        effect->done = 1;
        if (ended) *ended = 1;
        return 1;
    }

    if (next >= (double)effect->samples) {
        if (effect->loop >= 0) {
            double span = (double)effect->samples - (double)effect->loop;
            effect->position = (double)effect->loop +
                fmod(next - (double)effect->samples, span);
        } else {
            effect->done = 1;
            if (ended) *ended = 1;
        }
    } else {
        effect->position = next;
    }
    return 1;
}

void dc_spu_effect_retire(unsigned voice)
{
    if (voice < DC_SPU_EFFECT_VOICE_COUNT) clear_effect(&effects[voice]);
}

int dc_spu_effect_pcm_busy(const int16_t *pcm)
{
    if (!pcm) return 0;
    for (unsigned i = 0; i < DC_SPU_EFFECT_VOICE_COUNT; ++i)
        if (effects[i].pcm == pcm) return 1;
    return 0;
}
