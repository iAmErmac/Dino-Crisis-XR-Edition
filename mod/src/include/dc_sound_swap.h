#ifndef DC_SOUND_SWAP_H
#define DC_SOUND_SWAP_H
#include <stdint.h>
#include "cpu_state.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct DcSoundSwap {
    uint32_t entry_address, table_address, entry_word, table_word, page;
} DcSoundSwap;

int dc_fk_sound_prepare(CPUState *cpu, DcSoundSwap *swap);
int dc_sound_swap_valid(const DcSoundSwap *swap);
void dc_sound_swap_restore(DcSoundSwap *swap);
void dc_sound_capture_page(uint32_t page);
#ifdef __cplusplus
}
#endif
#endif
