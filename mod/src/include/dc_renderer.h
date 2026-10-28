#ifndef DC_RENDERER_H
#define DC_RENDERER_H
#ifdef __cplusplus
extern "C" {
#endif
void sw_set_pgxp_w(float w0, float w1, float w2, int valid);
void sw_set_pgxp_pos(float x0, float y0, float x1, float y1,
    float x2, float y2, int valid);
#ifdef __cplusplus
}
#endif
#endif
