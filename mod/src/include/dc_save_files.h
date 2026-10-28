#ifndef DC_SAVE_FILES_H
#define DC_SAVE_FILES_H

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

int dc_save_slot(void);
void dc_save_slot_set(int slot);
int dc_save_slot_count(void);
const char* dc_save_path(void);

int dc_file_finish(FILE* file);
int dc_replace_file(const char* temporary_path, const char* destination_path);

#ifdef __cplusplus
}
#endif

#endif
