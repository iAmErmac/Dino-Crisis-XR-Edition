#ifndef DC_SAVE_SESSION_H
#define DC_SAVE_SESSION_H
#include <stddef.h>
#include "cpu_state.h"
#ifdef __cplusplus
extern "C" {
#endif
void dc_save_session_configure(uint32_t checksum, uint32_t entry);
int dc_save_session_request(int load);
int dc_save_session_request_slot(int load, int slot);
int dc_save_session_pending(void);
const char *dc_save_session_path(void);
void dc_save_session_status_json(char *out, size_t cap);
void dc_save_session_poll(CPUState *cpu, uint32_t resume_pc);
void dc_save_session_loaded(void);
void dc_save_session_notify(int load, int slot, int ok);
#ifdef __cplusplus
}
#endif
#endif
