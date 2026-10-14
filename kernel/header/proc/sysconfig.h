// ============================================================
// CreateOS 0.0.1
// ============================================================

#ifndef SYSCONFIG_H
#define SYSCONFIG_H

int  sysconfig_load(void);
int  sysconfig_save(void);

int  sysconfig_cmd_get(const char* name);
void sysconfig_cmd_set(const char* name, int enabled);

int  sysconfig_goddo_locked(void);
void sysconfig_set_goddo_locked(int locked);

#endif
