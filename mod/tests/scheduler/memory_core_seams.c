#include <stdlib.h>
#include <assert.h>
#include "psx_bios_image.h"
#include "psx_memory.h"
#ifdef _MSC_VER
#define __builtin_trap() abort()
#endif
/* The linked-list walker is linked from its real implementation. */
#define dma_gpu_ll_start unused_dma_gpu_ll_start
#define dma_gpu_ll_cancel unused_dma_gpu_ll_cancel
#define dma_gpu_ll_cycles_to_event unused_dma_gpu_ll_cycles_to_event
#define dma_gpu_ll_advance unused_dma_gpu_ll_advance
#define psx_fatal_halt unused_memory_fatal_halt
#include "psx_ram_runtime_map_stubs.c"
#undef psx_fatal_halt
extern int dc_test_reject_render_reads;
void psx_fatal_halt(const char *reason) {
    if (dc_test_reject_render_reads && !strcmp(reason, "Device reads require the normal guest execution context")) exit(85);
    abort();
}
#undef dma_gpu_ll_start
#undef dma_gpu_ll_cancel
#undef dma_gpu_ll_cycles_to_event
#undef dma_gpu_ll_advance

int g_psx_cyc_bb_defer, g_psx_gcs_batch;
uint64_t g_psx_cycle_fast_limit;
int g_dma_exec_depth;
int g_dma_cur_ch;
uint32_t g_dma_cur_madr, g_dma_cur_bcr;
uint32_t g_dma_initiator_pc;
uint32_t g_overlay_region_floor, g_text_image_lo;
const PsxKernelBody *psx_bios_kernel_bodies;
uint32_t psx_bios_kernel_body_count;
const PsxKernelPatchRange *psx_bios_kernel_patch_ranges;
uint32_t psx_bios_kernel_patch_range_count;
PsxBiosImageInfo psx_bios_image;

/* Store policy is exercised separately; this fixture checks address routing. */
void dc_memory_policy_bind(void) { assert(memory_get_ram_bytes() == 0x200000u); }
int dc_memory_policy_write(uint32_t phys, uint32_t *value, uint32_t width)
{
    assert(phys == 0x1000 && width == 4 && *value == 0x12345678u);
    return 1;
}
uint32_t dma_read(uint32_t addr) { (void)addr; abort(); }
void dma_write(uint32_t addr, uint32_t value) { (void)addr; (void)value; abort(); }
void dma_write_masked(uint32_t addr, uint32_t value, uint32_t mask)
{ (void)addr; (void)value; (void)mask; abort(); }
void overlay_loader_active_write_check(uint32_t phys, uint32_t size)
{ (void)phys; (void)size; abort(); }
int gte_geometry_correction_lookup(uint32_t value, int32_t *x, int32_t *y, uint16_t *z)
{ (void)value; (void)x; (void)y; (void)z; abort(); }
int gte_geometry_correction_lookup_probe(uint32_t value, int32_t *x, int32_t *y, uint16_t *z)
{ (void)value; (void)x; (void)y; (void)z; abort(); }
int pt_poolgeom_is_pool(uint32_t address) { (void)address; abort(); }
void pt_poolgeom_vertex(uint32_t address, float x, float y)
{ (void)address; (void)x; (void)y; abort(); }
int pgxp_lookup(int x, int y, float *fx, float *fy, float *w)
{ (void)x; (void)y; (void)fx; (void)fy; (void)w; abort(); }

uint64_t s_frame_count;
