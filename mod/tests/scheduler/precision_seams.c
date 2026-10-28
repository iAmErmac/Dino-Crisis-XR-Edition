#include <stdint.h>
#include <stdlib.h>
uint32_t g_psx_ram_mask = 0x1FFFFFu;
int psx_mod_gpu_dma_memory_contains(uint32_t address, uint32_t size)
{ (void)address; (void)size; return 0; }
int gte_geometry_correction_lookup(uint32_t value, int32_t *x, int32_t *y, uint16_t *z)
{ (void)value; (void)x; (void)y; (void)z; abort(); }
int gte_geometry_correction_lookup_probe(uint32_t value, int32_t *x, int32_t *y)
{ (void)value; (void)x; (void)y; abort(); }
int pt_poolgeom_is_pool(uint32_t address) { (void)address; return 0; }
void pt_poolgeom_vertex(uint32_t value, float x, float y)
{ (void)value; (void)x; (void)y; abort(); }
int pgxp_lookup(int x, int y, float *fx, float *fy, float *w)
{ (void)x; (void)y; (void)fx; (void)fy; (void)w; abort(); }
