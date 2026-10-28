#include <stdlib.h>
#include <stdint.h>
void spu_xpage_capture(int page) { (void)page; abort(); }
#ifdef DC_SOUND_NO_RAM
uint8_t *memory_get_ram_ptr(void) { abort(); }
#endif
