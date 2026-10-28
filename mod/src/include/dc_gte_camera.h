#ifndef DC_GTE_CAMERA_H
#define DC_GTE_CAMERA_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct DcGteCameraVertex {
    float x, y, depth;
    uint32_t sz;
    int active, rewrite_mac;
} DcGteCameraVertex;
void dc_gte_camera_begin(int triple, const int16_t rotation[9], const int32_t translation[3]);
void dc_gte_camera_end(int triple, const int16_t rotation[9], const int32_t translation[3]);
void dc_gte_camera_reset(void);
DcGteCameraVertex dc_gte_camera_view(int64_t x, int64_t y, int64_t z);
void dc_gte_camera_aspect(int64_t *num, int64_t *den);
int dc_gte_camera_unclip(void);
extern int g_dc_prim_crossed;
#ifdef __cplusplus
}
#endif
#endif
