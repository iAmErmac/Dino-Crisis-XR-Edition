#ifdef DC_TEST_KERNEL_BACKEND
#define g_psx_dispatch_depth fixture_dispatch_depth
#endif
#define psx_read_word fixture_read_word
#define spu_ctrl_read fixture_spu_ctrl_read
#include "restore_edge_irq_stubs.c"
#undef spu_ctrl_read
#undef psx_read_word
#ifdef DC_TEST_KERNEL_BACKEND
#undef g_psx_dispatch_depth
#endif

uint16_t g_test_spu_ctrl;
uint16_t spu_ctrl_read(void) { return g_test_spu_ctrl; }

/* The scheduler fixtures run the identity guest-cycle scale. */
uint32_t g_psx_gcs_recip_q16 = 65536u;
uint32_t g_psx_gcs_frac = 0u;

uint32_t psx_read_word(uint32_t address)
{
    if (address == 0x1F801070u) return i_stat;
    if (address == 0x1F801074u) return i_mask;
    return fixture_read_word(address);
}

void psx_write_word(uint32_t address, uint32_t value)
{
    if (address == 0x1F801070u) i_stat &= value;
    else if (address == 0x1F801074u) i_mask = value;
    else psx_host_write_word(address, value);
}

/* Unexercised kernel services must not conceal an unexpected path. */
#define UNREACHED(name) void name(void) { abort(); }
#ifdef DC_TEST_BOOT_CONTRACT
void dirty_ram_mark_executable_range(uint32_t phys, uint32_t len)
{ (void)phys; (void)len; }
#else
UNREACHED(dirty_ram_mark_executable_range)
#endif
UNREACHED(fntrace_is_game_started)
UNREACHED(cdrom_streaming)
UNREACHED(sio_get_pad_buttons)
#ifndef DC_TEST_CARD
#ifndef DC_REAL_MEMCARD
UNREACHED(memcard_read_sector)
UNREACHED(memcard_write_sector)
UNREACHED(memcard_is_present)
UNREACHED(memcard_flush)
UNREACHED(memcard_last_flush_result)
#endif
#endif
#ifdef DC_TEST_BOOT_CONTRACT
int dc_mods_exe_patch(uint32_t t_addr, uint8_t *body, uint32_t size)
{ (void)t_addr; (void)body; (void)size; return 0; }
#else
UNREACHED(dc_mods_exe_patch)
#endif
volatile uint32_t g_dc_fn_ring[32], g_dc_fn_pos;
