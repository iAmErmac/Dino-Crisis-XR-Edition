#include "dc_gte_capture.h"
#include "dc_projection_pairs.h"
#include <limits.h>
#include <string.h>
#include <stdlib.h>

extern int g_pvx_window;
extern void pt_poolgeom_rtps(uint32_t packed, float x, float y, float depth);
float g_cam_R[9], g_cam_T[3];
int g_cam_have;
static int16_t s_rotation[9];
static int32_t s_translation[3];
static int s_epoch;

void dc_gte_capture_reset(void)
{
    memset(s_rotation, 0, sizeof s_rotation);
    memset(s_translation, 0, sizeof s_translation);
    memset(g_cam_R, 0, sizeof g_cam_R);
    memset(g_cam_T, 0, sizeof g_cam_T);
    s_epoch = 0; g_cam_have = 0;
    dc_gte_light_reset();
    dc_pair_projection_reset();
}

int pt_current_matrix_epoch(void) { return s_epoch; }

void pt_gte_read_matrix(CPUState *cpu, float rotation[9], float translation[3])
{
    for (unsigned i = 0; i < 9; ++i) {
        uint32_t value = cpu->gte_ctrl[i / 2] >> ((i & 1u) * 16u);
        rotation[i] = (float)(int16_t)value / 4096.0f;
    }
    for (unsigned i = 0; i < 3; ++i) translation[i] = (float)(int32_t)cpu->gte_ctrl[5 + i];
}

void dc_gte_capture_projection(const int16_t rotation[9], const int32_t translation[3],
    uint32_t packed, float x, float y, float depth, int valid)
{
    if (memcmp(s_rotation, rotation, sizeof s_rotation) ||
        memcmp(s_translation, translation, sizeof s_translation)) {
        memcpy(s_rotation, rotation, sizeof s_rotation);
        memcpy(s_translation, translation, sizeof s_translation);
        s_epoch = s_epoch == INT_MAX ? 1 : s_epoch + 1;
    }
    if (!valid) return;
    pt_poolgeom_rtps(packed, x, y, depth);
    /* The pool consumer closes its window on the final body vertex. */
    if (g_pvx_window <= 0 && depth > 0.0f) {
        for (unsigned i = 0; i < 9; ++i) g_cam_R[i] = (float)rotation[i] / 4096.0f;
        for (unsigned i = 0; i < 3; ++i) g_cam_T[i] = (float)translation[i];
        g_cam_have = 1;
    }
}

typedef struct DcLightCapture {
    int16_t light[9], color[9], rotation[9];
    int32_t translation[3];
    long hits;
} DcLightCapture;
static DcLightCapture lights[8];
static int light_count, light_enabled = -1;

void dc_gte_light_reset(void) { light_count = 0; }

void dc_gte_light_note(const CPUState *cpu)
{
    if (light_enabled < 0) {
        const char *value = getenv("DC_LIGHT_GTE");
        light_enabled = value && value[0] == '1';
    }
    if (!light_enabled || !cpu) return;
    DcLightCapture entry;
    for (unsigned i = 0; i < 9; ++i) {
        unsigned shift = (i & 1u) * 16u;
        entry.light[i] = (int16_t)(cpu->gte_ctrl[8u + i / 2u] >> shift);
        entry.color[i] = (int16_t)(cpu->gte_ctrl[16u + i / 2u] >> shift);
        entry.rotation[i] = (int16_t)(cpu->gte_ctrl[i / 2u] >> shift);
    }
    for (unsigned i = 0; i < 3; ++i) entry.translation[i] = (int32_t)cpu->gte_ctrl[5u + i];
    for (int i = 0; i < light_count; ++i) {
        if (!memcmp(lights[i].light, entry.light, sizeof entry.light) &&
            !memcmp(lights[i].translation, entry.translation, sizeof entry.translation)) {
            if (lights[i].hits < LONG_MAX) ++lights[i].hits;
            return;
        }
    }
    if (light_count == 8) return;
    entry.hits = 1; lights[light_count++] = entry;
}

int dc_gte_light_get(float direction[3], float color[3])
{
    if (!direction || !color) return 0;
    int best = -1;
    for (int i = 0; i < light_count; ++i) {
        if (!(lights[i].light[0] || lights[i].light[1] || lights[i].light[2])) continue;
        if (best < 0 || lights[i].hits > lights[best].hits) best = i;
    }
    if (best < 0) return 0;
    for (unsigned i = 0; i < 3; ++i) {
        direction[i] = lights[best].light[i] / 4096.0f;
        color[i] = lights[best].color[i * 3u] / 4096.0f;
    }
    return 1;
}

int dc_gte_light_snap(int index, float light[9], float color[9], float rotation[9], float translation[3], long *hits)
{
    if (index < 0 || index >= light_count || !light || !color || !rotation || !translation) return 0;
    const DcLightCapture *entry = &lights[index];
    for (unsigned i = 0; i < 9; ++i) {
        light[i] = entry->light[i] / 4096.0f;
        color[i] = entry->color[i] / 4096.0f;
        rotation[i] = entry->rotation[i] / 4096.0f;
    }
    for (unsigned i = 0; i < 3; ++i) translation[i] = (float)entry->translation[i];
    if (hits) *hits = entry->hits;
    return 1;
}
