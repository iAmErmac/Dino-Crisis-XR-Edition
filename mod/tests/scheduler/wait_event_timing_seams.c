#ifdef DC_TEST_KERNEL_BACKEND
#define g_psx_dispatch_depth fixture_dispatch_depth
#endif
#define psx_read_word fixture_read_word
#define psx_devices_service_to_now fixture_noop_device_service
#include "restore_edge_irq_stubs.c"
#undef psx_read_word
#undef psx_devices_service_to_now
#ifdef DC_TEST_KERNEL_BACKEND
#undef g_psx_dispatch_depth
#endif

uint32_t g_psx_gcs_recip_q16 = 65536u;
uint32_t g_psx_gcs_frac = 0u;

void wait_event_timing_service(void);

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

void psx_devices_service_to_now(void)
{
    wait_event_timing_service();
}

/* The timing test boots the same minimal EXE contract as the boot test. */
#define UNREACHED(name) void name(void) { abort(); }
void dirty_ram_mark_executable_range(uint32_t phys, uint32_t len)
{ (void)phys; (void)len; }
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
int dc_mods_exe_patch(uint32_t t_addr, uint8_t *body, uint32_t size)
{ (void)t_addr; (void)body; (void)size; return 0; }
volatile uint32_t g_dc_fn_ring[32], g_dc_fn_pos;
