#include "dc_gte_camera.h"
#include "crash_trace.h"
#include <math.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

extern void dc_nots_rtps_tick(void);
extern int dc_nots_get_delta(float rotation[9], float translation[3]);
extern int dc_ws_world(void);
extern void dc_ws_frac(int *num, int *den);
int g_dc_prim_crossed;
static int s_polygon, s_last_triple, s_active;
static int16_t s_rotation[9];
static int32_t s_translation[3];
static int s_rewrite = -1, s_unclip = -1, s_depth_limit = INT_MIN;
static int option_on(const char *name, int *cached)
{
    if (*cached < 0) {
        const char *value = getenv(name);
        *cached = !value || *value != '0';
    }
    return *cached;
}

void dc_gte_camera_reset(void)
{
    s_polygon = s_last_triple = s_active = g_dc_prim_crossed = 0;
    memset(s_rotation, 0, sizeof s_rotation);
    memset(s_translation, 0, sizeof s_translation);
}
void dc_gte_camera_begin(int triple, const int16_t rotation[9], const int32_t translation[3])
{
    s_polygon = triple || (s_last_triple && !memcmp(rotation, s_rotation, sizeof s_rotation) &&
        !memcmp(translation, s_translation, sizeof s_translation));
    s_active = 0;
    if (!triple && !s_polygon) s_last_triple = 0;
    if (triple) g_dc_prim_crossed = 0;
}
void dc_gte_camera_end(int triple, const int16_t rotation[9], const int32_t translation[3])
{
    if (triple) {
        memcpy(s_rotation, rotation, sizeof s_rotation);
        memcpy(s_translation, translation, sizeof s_translation);
        s_last_triple = 1;
    }
    s_polygon = 0;
}

DcGteCameraVertex dc_gte_camera_view(int64_t x, int64_t y, int64_t z)
{
    DcGteCameraVertex result = {0}; float rotation[9], translation[3];
    dc_nots_rtps_tick();
    if (!dc_nots_get_delta(rotation, translation)) { s_active = 0; return result; }
    double view[3] = {(double)x / 4096.0, (double)y / 4096.0, (double)z / 4096.0};
    float projected[3];
    for (unsigned r = 0; r < 3; ++r) {
        double value = translation[r];
        for (unsigned c = 0; c < 3; ++c) value += rotation[r * 3 + c] * view[c];
        if (!isfinite(value) || value < -2147483000.0 || value > 2147483000.0) {
            psx_fatal_halt("gte: invalid camera transform"); abort();
        }
        projected[r] = (float)value;
    }
    result.x = fminf(32767.0f, fmaxf(-32768.0f, projected[0]));
    result.y = fminf(32767.0f, fmaxf(-32768.0f, projected[1]));
    result.depth = projected[2];
    if (result.depth < 1.0f) g_dc_prim_crossed = 1;
    double depth = fmax(0.0, (double)result.depth);
    if (s_depth_limit == INT_MIN) {
        const char *limit = getenv("DC_SZ_CLAMP");
        s_depth_limit = limit ? atoi(limit) : 0;
    }
    int maximum = s_depth_limit;
    if (maximum > 0 && depth > maximum) depth = maximum;
    result.sz = (uint32_t)depth;
    result.active = 1; result.rewrite_mac = s_polygon && option_on("DC_REAIM_MACIR", &s_rewrite);
    s_active = 1;
    return result;
}

void dc_gte_camera_aspect(int64_t *num, int64_t *den)
{
    int numerator = 1, denominator = 1;
    if (dc_ws_world()) dc_ws_frac(&numerator, &denominator);
    if (numerator <= 0 || denominator <= 0) {
        psx_fatal_halt("gte: invalid display aspect"); abort();
    }
    *num = numerator; *den = denominator;
}
int dc_gte_camera_unclip(void) { return s_active && option_on("DC_UNCLIP", &s_unclip); }
