#include <stdint.h>

uint64_t psx_cycle_count;
uint64_t psx_next_service_cycle;
uint32_t g_psx_gcs_recip_q16 = 65536u;
uint32_t g_psx_gcs_frac;
uint32_t g_psx_cyc_batch_limit;
int g_event_step_conservative;
int g_ls_replay_active;

void psx_devices_service_to_now(void) {}
void psx_advance_cycles_slow(uint32_t cycles) { psx_cycle_count += cycles; }
