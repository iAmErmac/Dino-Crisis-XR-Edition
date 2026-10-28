#include <setjmp.h>
#include <stdint.h>
#include <stdlib.h>

/* These paths are outside this fixture; reaching one must fail the test. */
#define UNREACHED(name) void name(void) { abort(); }
UNREACHED(psx_exception_longjmp)
UNREACHED(psx_crash_trace_dump)
UNREACHED(psx_fatal_halt)
UNREACHED(ds_note_thread_switch)
UNREACHED(overlay_loader_shadow_escape_fixup)
UNREACHED(crash_trace_dispatch_ring_get)
UNREACHED(crash_trace_dispatch_seq_get)
#if !defined(DC_TEST_SOUND) && !defined(DC_TEST_SHOT) && !defined(DC_TEST_CREATURE_BLOOD)
UNREACHED(memory_get_ram_ptr)
#endif
UNREACHED(psx_unknown_dispatch_record)
UNREACHED(psx_check_interrupts_dispatch_entry)

int g_rfe_escape_pending, g_exc_escape_reason;
uint32_t g_psx_ram_size = 2 * 1024 * 1024;
void *g_exception_owner_fiber;
int g_pending_exception_longjmp;
jmp_buf exception_jmpbuf;
uint64_t g_exc_setjmp_epoch;
uint32_t g_async_rfe_resume_pc;
uint64_t g_async_rfe_fire_count, g_sentinel_reach_traps, g_sentinel_reach_async;

#ifdef DC_TEST_KERNEL
UNREACHED(psx_rfe_mark_escape)
UNREACHED(psx_rfe_escape_check)
UNREACHED(psx_check_interrupts)
UNREACHED(psx_check_interrupts_at)
UNREACHED(dirty_ram_mark_executable_range)
UNREACHED(debug_server_poll)
UNREACHED(fntrace_is_game_started)
UNREACHED(cdrom_streaming)
UNREACHED(sio_get_pad_buttons)
UNREACHED(memcard_read_sector)
UNREACHED(memcard_write_sector)
UNREACHED(memcard_is_present)
UNREACHED(memcard_flush)
UNREACHED(memcard_last_flush_result)
UNREACHED(dc_mods_exe_patch)
uint32_t g_exception_real_epc;
volatile uint32_t g_dc_fn_ring[32], g_dc_fn_pos;
#endif
