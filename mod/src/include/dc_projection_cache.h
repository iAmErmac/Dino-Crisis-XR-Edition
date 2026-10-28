#ifndef DC_PROJECTION_CACHE_H
#define DC_PROJECTION_CACHE_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void dc_projection_cache_clear(void);
void dc_projection_cache_put(int x, int y, float precise_x, float precise_y, float depth);
int dc_projection_cache_get(int x, int y, float *precise_x, float *precise_y, float *depth);
void dc_projection_constants(uint32_t height, int32_t offset_x, int32_t offset_y);
extern float g_pt_proj_h, g_pt_proj_ofx, g_pt_proj_ofy;
#ifdef __cplusplus
}
#endif
#endif
