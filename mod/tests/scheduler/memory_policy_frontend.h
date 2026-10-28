#ifndef DC_MEMORY_POLICY_FRONTEND_H
#define DC_MEMORY_POLICY_FRONTEND_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void memory_policy_frontend_reset(void);
int memory_policy_frontend_drawlist_calls(void);
int memory_policy_frontend_ammo_calls(void);
int memory_policy_frontend_ammo_old(unsigned index);
int memory_policy_frontend_ammo_new(unsigned index);
int memory_policy_frontend_damage_calls(void);
int memory_policy_frontend_damage_slot(void);
int memory_policy_frontend_damage_old(void);
int memory_policy_frontend_damage_new(void);
float memory_policy_frontend_damage_scale(void);

extern int g_opt_focus_mode;

#ifdef __cplusplus
}
#endif

#endif
