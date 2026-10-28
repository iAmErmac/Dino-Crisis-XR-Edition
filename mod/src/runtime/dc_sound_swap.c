#include "dc_sound_swap.h"
#include <string.h>
#include <stdlib.h>
extern uint8_t *memory_get_ram_ptr(void);
extern void spu_xpage_capture(int page);
extern void psx_fatal_halt(const char *reason);

int dc_sound_swap_valid(const DcSoundSwap *swap)
{
    return !swap->page || (swap->page <= 8 &&
        swap->entry_address >= 0x1FE9C0u && swap->entry_address <= 0x1FEC7Cu &&
        !(swap->entry_address & 3u) && swap->table_address >= 0x9E538u &&
        swap->table_address <= 0x9E554u && !(swap->table_address & 3u));
}

void dc_sound_capture_page(uint32_t page)
{
    spu_xpage_capture((int)page);
}

void dc_sound_swap_restore(DcSoundSwap *swap)
{
    if (!swap->page) return;
    uint8_t *ram = memory_get_ram_ptr();
    if (!ram || !dc_sound_swap_valid(swap)) {
        psx_fatal_halt("sound swap: invalid restore state");
        abort();
    }
    dc_sound_capture_page(0);
    memcpy(ram + swap->entry_address, &swap->entry_word, 4);
    memcpy(ram + swap->table_address, &swap->table_word, 4);
    memset(swap, 0, sizeof *swap);
}
