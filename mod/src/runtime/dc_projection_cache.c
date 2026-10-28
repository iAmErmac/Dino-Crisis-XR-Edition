#include "dc_projection_cache.h"
#include <math.h>
#include <string.h>

extern uint64_t s_frame_count;
float g_pt_proj_h, g_pt_proj_ofx, g_pt_proj_ofy;

enum { CACHE_SIZE = 131072, CACHE_PROBES = 8 };
typedef struct ProjectionEntry {
    uint32_t key, frame, valid;
    float x, y, depth;
} ProjectionEntry;
static ProjectionEntry entries[CACHE_SIZE];

static uint32_t cache_key(int x, int y)
{
    return ((uint32_t)x & 0xFFFFu) << 16 | ((uint32_t)y & 0xFFFFu);
}
static uint32_t cache_slot(uint32_t key)
{
    uint32_t hash = key * 2654435761u;
    return (hash ^ (hash >> 15)) & (CACHE_SIZE - 1u);
}
void dc_projection_cache_clear(void) { memset(entries, 0, sizeof entries); }
void dc_projection_constants(uint32_t height, int32_t offset_x, int32_t offset_y)
{
    g_pt_proj_h = (float)height;
    g_pt_proj_ofx = (float)((double)offset_x / 65536.0);
    g_pt_proj_ofy = (float)((double)offset_y / 65536.0);
}
void dc_projection_cache_put(int x, int y, float precise_x, float precise_y, float depth)
{
    if (!isfinite(precise_x) || !isfinite(precise_y) || !isfinite(depth)) return;
    uint32_t key = cache_key(x, y), slot = cache_slot(key), now = (uint32_t)s_frame_count;
    ProjectionEntry *chosen = NULL, *stale = NULL;
    for (unsigned i = 0; i < CACHE_PROBES; ++i) {
        ProjectionEntry *entry = &entries[(slot + i) & (CACHE_SIZE - 1u)];
        if (!entry->valid || entry->key == key) { chosen = entry; break; }
        if (!stale && now - entry->frame > 4u) stale = entry;
    }
    if (!chosen) chosen = stale ? stale : &entries[slot];
    chosen->key = key; chosen->frame = now; chosen->valid = 1;
    chosen->x = precise_x; chosen->y = precise_y; chosen->depth = depth;
}
int dc_projection_cache_get(int x, int y, float *precise_x, float *precise_y, float *depth)
{
    if (!precise_x || !precise_y || !depth) return 0;
    uint32_t key = cache_key(x, y), slot = cache_slot(key), now = (uint32_t)s_frame_count;
    for (unsigned i = 0; i < CACHE_PROBES; ++i) {
        const ProjectionEntry *entry = &entries[(slot + i) & (CACHE_SIZE - 1u)];
        if (!entry->valid) break;
        if (entry->key != key) continue;
        /* One previous 30 Hz game frame remains useful to the depth path. */
        if (now - entry->frame > 3u) break;
        *precise_x = entry->x; *precise_y = entry->y; *depth = entry->depth;
        return 1;
    }
    return 0;
}
