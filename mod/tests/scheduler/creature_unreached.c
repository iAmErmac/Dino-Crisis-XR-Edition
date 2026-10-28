#include <stdint.h>
#include <stdlib.h>
#include "dc_shot.h"

/* These fixtures do not dispatch creature callbacks. */
uint32_t g_debug_last_store_pc;
uint32_t dc_fk_bank(uint32_t pc) { (void)pc; abort(); }
int dc_actor_model_bones(const uint8_t *ram, uint32_t actor) { (void)ram; (void)actor; abort(); }
int dc_recent_shot_victim(int slot) { (void)slot; abort(); }
int dc_blood_mult(void) { abort(); }
int dc_blood_pool_room(void) { abort(); }
uint32_t dc_rng_seed_get(void) { abort(); }
void dc_rng_seed_set(uint32_t seed) { (void)seed; abort(); }
