#include <stdint.h>
#include <stdlib.h>

#if !defined(DC_SOUND_NO_RAM) && !defined(DC_TEST_KERNEL_BACKEND)
uint8_t *memory_get_ram_ptr(void) { abort(); }
#endif

int dc_nots_active(void) { abort(); }
int dc_scripted_now(void) { abort(); }
int dc_scene_flag_on(void) { abort(); }
