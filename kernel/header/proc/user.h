// ============================================================
// CreateOS 0.0.1
// ============================================================

#ifndef KERNEL_USER_H
#define KERNEL_USER_H

#include <stdint.h>

#define USER_ADD_SUCCESS 1
#define USER_ADD_INVALID_INPUT (-1)
#define USER_ADD_PERMISSION_DENIED (-2)
#define USER_ADD_LIMIT_REACHED (-3)
#define USER_ADD_ALREADY_EXISTS (-4)
#define USER_ADD_NO_SECURE_RANDOM (-5)
#define USER_ADD_DATABASE_FAILED (-6)
#define USER_LOGIN_PROFILE_ERROR (-2)

typedef struct {
    char name[32];
    char password_hash[32];
    int role;
    char home[64];
    int locked;
} kernel_user_t;

void kernel_users_init(void);
int kernel_user_add(const char* name, const char* password, int role);
int kernel_user_count(void);
int kernel_user_get(int index, kernel_user_t* out);
int kernel_user_find(const char* name, kernel_user_t** out);
int kernel_user_login(const char* name, const char* password);
#ifdef PROC_CONTEXT_SELFTEST
int kernel_user_test_login_rejects_active_identity(void);
#endif
int kernel_user_verify_current_password(const char* password);
void kernel_user_logout(void);
kernel_user_t* kernel_user_current(void);
int kernel_user_setpass(const char* name, const char* password);
int kernel_user_set_locked(const char* name, int locked);
int kernel_user_set_disabled(const char* name, int disabled);
int kernel_user_set_role(const char* name, int role);
int kernel_user_is_admin(void);
int kernel_user_uid(const kernel_user_t* user);

#endif