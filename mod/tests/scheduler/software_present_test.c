#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <SDL.h>
#include "gpu_sw_renderer.h"
#include "test_process.h"

static uint16_t vram[1024 * 512], before[1024 * 512];
int g_ws_bd_stretch_on, g_ws_bd_stretch_pct;
int psx_ws_prim_in_backdrop(void)
{
    assert(!"Unexpected backdrop classification during display conversion");
    return 0;
}

static void test_display(int scale, int wrap)
{
    memset(vram, 0, sizeof vram); sw_renderer_init(vram); sw_renderer_set_scale(scale);
    assert(sw_renderer_scale() == scale);
    int x = wrap ? 1023 : 3, y = wrap ? 511 : 2;
    sw_vram_write(x, y, 0x001F); sw_vram_write((x + 1) & 1023, y, 0x03E0);
    memcpy(before, vram, sizeof before);
    const int width = 2 * scale, height = scale, stride = width + 2;
    uint32_t pixels[48], readback[32];
    for (unsigned i = 0; i < 48; ++i) pixels[i] = 0x12345678u;
    assert(sw_render_display_hires(pixels, stride * 4, x, y, 2, 1) == width * height);
    for (int row = 0; row < height; ++row) {
        for (int col = 0; col < width; ++col)
            assert(pixels[row * stride + col] == (col < scale ? 0xFFFF0000u : 0xFF00FF00u));
        assert(pixels[row * stride + width] == 0x12345678u);
        assert(pixels[row * stride + width + 1] == 0x12345678u);
    }
    assert(!memcmp(before, vram, sizeof before));
    SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormat(0, width, height, 32, SDL_PIXELFORMAT_ARGB8888);
    assert(surface); SDL_Renderer *renderer = SDL_CreateSoftwareRenderer(surface); assert(renderer);
    SDL_Texture *texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                             SDL_TEXTUREACCESS_STREAMING, width, height);
    assert(texture && SDL_UpdateTexture(texture, NULL, pixels, stride * 4) == 0);
    assert(SDL_RenderCopy(renderer, texture, NULL, NULL) == 0);
    assert(SDL_RenderReadPixels(renderer, NULL, SDL_PIXELFORMAT_ARGB8888, readback, width * 4) == 0);
    for (int row = 0; row < height; ++row)
        assert(!memcmp(readback + row * width, pixels + row * stride, (size_t)width * 4));
    SDL_DestroyTexture(texture); SDL_DestroyRenderer(renderer); SDL_FreeSurface(surface);
}

int main(void)
{
    test_process_init(); assert(SDL_Init(SDL_INIT_VIDEO) == 0);
    for (int scale = 1; scale <= 4; scale *= 2) {
        test_display(scale, 0); test_display(scale, 1);
    }
    sw_renderer_set_scale(1); SDL_Quit();
    puts("PASS: actual scaled software scanout, wrapped crop, pitch and SDL ARGB texture transfer");
    return 0;
}
