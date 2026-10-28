#include <assert.h>
#include <math.h>
#include <stdint.h>
#include "dc_projection_cache.h"
#include "test_process.h"
uint64_t s_frame_count;

int main(void)
{
    test_process_init();
    float x = -1, y = -2, z = -3;
    dc_projection_cache_clear();
    assert(!dc_projection_cache_get(-10, 20, &x, &y, &z));
    assert(x == -1 && y == -2 && z == -3);
    s_frame_count = 100;
    dc_projection_cache_put(-10, 20, -9.75f, 20.25f, 1234);
    assert(dc_projection_cache_get(-10, 20, &x, &y, &z));
    assert(x == -9.75f && y == 20.25f && z == 1234);
    dc_projection_cache_put(-10, 20, -9.5f, 20.5f, 2345);
    s_frame_count = 103;
    assert(dc_projection_cache_get(-10, 20, &x, &y, &z) && z == 2345);
    s_frame_count = 104; x = -1; y = -2; z = -3;
    assert(!dc_projection_cache_get(-10, 20, &x, &y, &z));
    assert(x == -1 && y == -2 && z == -3);
    dc_projection_cache_clear(); s_frame_count = UINT32_MAX - 1u;
    dc_projection_cache_put(-32768, 32767, -32767.5f, 32767.25f, 100);
    s_frame_count = 0;
    assert(dc_projection_cache_get(-32768, 32767, &x, &y, &z));
    s_frame_count = 2;
    assert(!dc_projection_cache_get(-32768, 32767, &x, &y, &z));
    dc_projection_cache_clear(); s_frame_count = 0;
    /* These distinct screen keys share a bucket in the retained hash layout. */
    const uint32_t collisions[] = {0, 117707, 215271, 332978, 430542, 645813, 743377, 861084, 958648};
    for (unsigned i = 0; i < 8; ++i)
        dc_projection_cache_put((int16_t)(collisions[i] >> 16), (int16_t)collisions[i], (float)i, 2, 3);
    for (unsigned i = 0; i < 8; ++i) {
        assert(dc_projection_cache_get((int16_t)(collisions[i] >> 16), (int16_t)collisions[i], &x, &y, &z));
        assert(x == (float)i);
    }
    s_frame_count = 5;
    dc_projection_cache_put(0, 0, 99, 2, 3);
    dc_projection_cache_put((int16_t)(collisions[8] >> 16), (int16_t)collisions[8], 88, 2, 3);
    assert(dc_projection_cache_get(0, 0, &x, &y, &z) && x == 99);
    assert(dc_projection_cache_get((int16_t)(collisions[8] >> 16), (int16_t)collisions[8], &x, &y, &z) && x == 88);
    dc_projection_cache_put(0, 0, NAN, 2, 3);
    assert(dc_projection_cache_get(0, 0, &x, &y, &z) && x == 99);
    dc_projection_cache_put(0, 0, 1, 2, -3);
    assert(dc_projection_cache_get(0, 0, &x, &y, &z) && z == -3);
    assert(!dc_projection_cache_get(0, 0, NULL, &y, &z));
    dc_projection_constants(320, -32768, 65536 * 120);
    assert(g_pt_proj_h == 320 && g_pt_proj_ofx == -0.5f && g_pt_proj_ofy == 120);
    dc_projection_cache_clear();
    assert(!dc_projection_cache_get(0, 0, &x, &y, &z));
    assert(g_pt_proj_h == 320);
    return 0;
}
