// ============================================================
// CreateOS 0.0.1
// ============================================================

#include "proc/user.h"
#include "proc/user_name.h"
#include "drivers/tty.h"
#include "drivers/keyboard.h"
#include "drivers/serial.h"
#include "fs/fs.h"
#include "lib/hash.h"
#include "lib/string.h"
#include <stddef.h>
#include "lib/stdlib.h"

#define MAX_KERNEL_USERS 16
#define USER_DATABASE_BUFFER_SIZE 4096

static kernel_user_t users[MAX_KERNEL_USERS];
static int user_count;
static kernel_user_t* current_users[TTY_CONSOLE_COUNT];

static kernel_user_t** current_user_slot(void)
{
    int console = tty_execution_console();
    if (console < 0 || console >= TTY_CONSOLE_COUNT) console = 0;
    return &current_users[console];
}
static uint8_t user_disabled[MAX_KERNEL_USERS];
static uint8_t user_salts[MAX_KERNEL_USERS][16];
static uint8_t user_hashes[MAX_KERNEL_USERS][32];

#define USER_HOME_TREE_MAX_DEPTH 24
#define USER_HOME_TREE_MAX_NODES 2048

typedef struct {
    char path[128];
    uint32_t next_entry;
} user_home_walk_frame_t;

static void user_home(kernel_user_t* user);
static int keyboard_layout_name_valid(const char* name, size_t length);

static int prepare_home_ownership(const kernel_user_t* user, uint16_t uid)
{
    user_home_walk_frame_t stack[USER_HOME_TREE_MAX_DEPTH];
    char names[1][CFS5_MAX_NAME];
    fs_stat_t status;
    int depth = 0;
    uint32_t visited = 0;

    if (!fs_stat(user->home, &status))
        return 1;
    if (status.type != FS_TYPE_DIR ||
        !fs_chown(user->home, uid, uid)) {
        serial_puts("[users] ERROR: unable to assign home directory owner\n");
        return 0;
    }
    strcpy(stack[depth].path, user->home);
    stack[depth].next_entry = 0;
    depth++;
    visited++;

    while (depth) {
        user_home_walk_frame_t* frame = &stack[depth - 1];
        int count = fs_list_offset(frame->path, names, frame->next_entry, 1);
        if (count < 0) {
            serial_puts("[users] ERROR: failed to enumerate home directory\n");
            return 0;
        }
        if (!count) {
            depth--;
            continue;
        }
        frame->next_entry++;
        if (++visited > USER_HOME_TREE_MAX_NODES) {
            serial_puts("[users] ERROR: home directory exceeds ownership scan limit\n");
            return 0;
        }

        char child[128];
        size_t parent_length = strlen(frame->path);
        size_t name_length = strlen(names[0]);
        if (parent_length + (parent_length > 1) + name_length >=
            sizeof(child)) {
            serial_puts("[users] ERROR: home path exceeds ownership scan limit\n");
            return 0;
        }
        strcpy(child, frame->path);
        if (parent_length > 1)
            strcat(child, "/");
        strcat(child, names[0]);

        if (!fs_stat(child, &status) || !fs_chown(child, uid, uid)) {
            serial_puts("[users] ERROR: unable to assign home entry owner\n");
            return 0;
        }
        if (status.type == FS_TYPE_DIR) {
            if (depth >= USER_HOME_TREE_MAX_DEPTH) {
                serial_puts("[users] ERROR: home directory nesting exceeds limit\n");
                return 0;
            }
            strcpy(stack[depth].path, child);
            stack[depth].next_entry = 0;
            depth++;
        }
    }
    return 1;
}

static int valid_hex(const char* value, size_t length)
{
    if (!value || strlen(value) != length) return 0;
    for (size_t i = 0; i < length; i++) {
        char c = value[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F')))
            return 0;
    }
    return 1;
}

static int valid_password(const char* password)
{
    size_t length;
    if (!password) return 0;
    length = strlen(password);
    return length > 0 && length < 64;
}

static void user_home(kernel_user_t* user)
{
    strcpy(user->home, "/home/");
    strcat(user->home, user->name);
}

static int keyboard_layout_name_valid(const char* name, size_t length)
{
    if (!name || length < 5 || length >= 64 ||
        memcmp(name + length - 4, ".lyt", 4) != 0)
        return 0;
    for (size_t i = 0; i < length - 4; i++) {
        char ch = name[i];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '-' || ch == '_'))
            return 0;
    }
    return 1;
}

static void load_users(void)
{
    char buffer[USER_DATABASE_BUFFER_SIZE];
    uint32_t size = 0;
    fs_stat_t status;

    if (!fs_stat("/etc/users.conf", &status))
        return;
    if (status.type != CFS5_INODE_FILE ||
        status.size >= sizeof(buffer)) {
        serial_puts("[users] ERROR: invalid or oversized account database\n");
        return;
    }
    if (!status.size)
        return;
    if (!fs_load_file("/etc/users.conf", (uint8_t*)buffer,
                      sizeof(buffer) - 1, &size) || size != status.size) {
        serial_puts("[users] ERROR: unable to read complete account database\n");
        return;
    }
    for (uint32_t i = 0; i < size; i++) {
        if (buffer[i] == 0) {
            serial_puts("[users] ERROR: account database contains binary data\n");
            return;
        }
    }
    buffer[size] = 0;

    char* line = buffer;
    int root_role_loaded = 0;
    while (*line) {
        char* end = strchr(line, '\n');
        if (end) *end = 0;
        size_t line_length = strlen(line);
        if (line_length && line[line_length - 1] == '\r')
            line[--line_length] = 0;

        char* fields[6] = { line, NULL, NULL, NULL, NULL, NULL };
        int field_count = 1;
        for (char* p = line; *p && field_count < 6; p++) {
            if (*p == ':') {
                *p = 0;
                fields[field_count++] = p + 1;
            }
        }

        if (line_length && line[0] != '#') {
            uint8_t salt[16];
            uint8_t hash[32];
            kernel_user_t* duplicate = NULL;
            int is_duplicate;
            int valid = field_count == 6 && user_name_valid(line) &&
                valid_hex(fields[1], 32) && valid_hex(fields[2], 64) &&
                fields[3][0] >= '0' && fields[3][0] <= '3' &&
                !fields[3][1] &&
                (fields[4][0] == '0' || fields[4][0] == '1') &&
                !fields[4][1] &&
                (fields[5][0] == '0' || fields[5][0] == '1') &&
                !fields[5][1] &&
                hex_to_salt(fields[1], salt) == 0 &&
                hex_to_hash(fields[2], hash) == 0;

            if (valid && fields[3][0] == '3' && root_role_loaded)
                valid = 0;
            is_duplicate = valid && kernel_user_find(line, &duplicate);
            if (!valid) {
                serial_puts("[users] ERROR: malformed account record ignored\n");
            } else if (is_duplicate) {
                serial_puts("[users] ERROR: duplicate account record ignored\n");
            } else if (user_count >= MAX_KERNEL_USERS) {
                serial_puts("[users] ERROR: account limit reached; remaining records ignored\n");
                break;
            } else {
                int index = user_count++;
                memset(&users[index], 0, sizeof(users[index]));
                strncpy(users[index].name, line, sizeof(users[index].name) - 1);
                users[index].role = fields[3][0] - '0';
                users[index].locked = fields[4][0] == '1';
                user_disabled[index] = fields[5][0] == '1';
                user_home(&users[index]);
                memcpy(user_salts[index], salt, sizeof(salt));
                memcpy(user_hashes[index], hash, sizeof(hash));
                if (users[index].role == 3)
                    root_role_loaded = 1;
            }
        }
        if (!end) break;
        line = end + 1;
    }
}

static int save_users(void)
{
    char buffer[USER_DATABASE_BUFFER_SIZE];
    int pos = 0;

    for (int i = 0; i < user_count; i++) {
        char salt[33];
        char hash[65];
        salt_to_hex(user_salts[i], salt);
        hash_to_hex(user_hashes[i], hash);
        int name_len = strlen(users[i].name);
        if (pos + name_len + 1 + 32 + 1 + 64 + 7 >= (int)sizeof(buffer))
            return 0;
        memcpy(buffer + pos, users[i].name, name_len);
        pos += name_len;
        buffer[pos++] = ':';
        memcpy(buffer + pos, salt, 32);
        pos += 32;
        buffer[pos++] = ':';
        memcpy(buffer + pos, hash, 64);
        pos += 64;
        buffer[pos++] = ':';
        buffer[pos++] = (char)('0' + users[i].role);
        buffer[pos++] = ':';
        buffer[pos++] = users[i].locked ? '1' : '0';
        buffer[pos++] = ':';
        buffer[pos++] = user_disabled[i] ? '1' : '0';
        buffer[pos++] = '\n';
    }
    if (!fs_write_file("/etc/users.conf", (const uint8_t*)buffer, pos)) {
        serial_puts("[users] ERROR: failed to persist account database\n");
        return 0;
    }
    return 1;
}

void kernel_users_init(void)
{
    user_count = 0;
    for (int i = 0; i < TTY_CONSOLE_COUNT; i++) current_users[i] = NULL;
    memset(user_disabled, 0, sizeof(user_disabled));
    load_users();
}

int kernel_user_find(const char* name, kernel_user_t** out)
{
    if (!name) return 0;
    for (int i = 0; i < user_count; i++) {
        if (user_name_equal(users[i].name, name)) {
            if (out) *out = &users[i];
            return 1;
        }
    }
    if (out) *out = NULL;
    return 0;
}

int kernel_user_count(void)
{
    return user_count;
}

int kernel_user_get(int index, kernel_user_t* out)
{
    if (!out || index < 0 || index >= user_count)
        return 0;
    *out = users[index];
    memset(out->password_hash, 0, sizeof(out->password_hash));
    return 1;
}

int kernel_user_add(const char* name, const char* password, int role)
{
    kernel_user_t* ignored;
    kernel_user_t* current = kernel_user_current();
    uint8_t salt[sizeof(user_salts[0])];
    if (!user_name_creation_valid(name) || !valid_password(password) ||
        role < 0 || role > 3)
        return USER_ADD_INVALID_INPUT;
    if (user_count >= MAX_KERNEL_USERS)
        return USER_ADD_LIMIT_REACHED;
    if (kernel_user_find(name, &ignored))
        return USER_ADD_ALREADY_EXISTS;
    if (current) {
        if (!kernel_user_is_admin() || role == 3 ||
            !strcmp(current->name, name))
            return USER_ADD_PERMISSION_DENIED;
    } else if (user_count != 0 || role != 3) {
        return USER_ADD_PERMISSION_DENIED;
    }

    if (!generate_salt(salt)) {
        serial_puts("[users] ERROR: secure random source unavailable\n");
        return USER_ADD_NO_SECURE_RANDOM;
    }

    int index = user_count++;
    memset(&users[index], 0, sizeof(users[index]));
    strncpy(users[index].name, name, sizeof(users[index].name) - 1);
    users[index].role = role;
    user_home(&users[index]);
    memcpy(user_salts[index], salt, sizeof(salt));
    hash_password(password, user_salts[index], user_hashes[index]);
    if (!save_users()) {
        serial_puts("[users] ERROR: unable to persist the account database\n");
        user_count--;
        memset(&users[index], 0, sizeof(users[index]));
        memset(user_salts[index], 0, sizeof(user_salts[index]));
        memset(user_hashes[index], 0, sizeof(user_hashes[index]));
        user_disabled[index] = 0;
        return USER_ADD_DATABASE_FAILED;
    }
    if (!current)
        *current_user_slot() = &users[index];
    return USER_ADD_SUCCESS;
}

int kernel_user_is_admin(void)
{
    kernel_user_t* current_user = *current_user_slot();
    return current_user && current_user->role >= 2;
}

int kernel_user_uid(const kernel_user_t* user)
{
    if (!user) return -1;
    if (user->role == 3) return 0;
    for (int i = 0; i < user_count; i++) {
        if (user == &users[i])
            return 1000 + i;
    }
    return -1;
}

int kernel_user_login(const char* name, const char* password)
{
    kernel_user_t* user;
    if (kernel_user_current() || !valid_password(password) ||
        !kernel_user_find(name, &user) || user->locked ||
        user_disabled[user - users])
        return 0;
    int index = (int)(user - users);
    if (!verify_password(password, user_salts[index], user_hashes[index]))
        return 0;
    if (kernel_user_uid(user) < 0) {
        serial_puts("[users] ERROR: account has no valid filesystem identity\n");
        return USER_LOGIN_PROFILE_ERROR;
    }
    if (kernel_user_uid(user) != 0 &&
        !prepare_home_ownership(user, (uint16_t)kernel_user_uid(user)))
        return USER_LOGIN_PROFILE_ERROR;
    *current_user_slot() = user;
    return 1;
}

#ifdef PROC_CONTEXT_SELFTEST
int kernel_user_test_login_rejects_active_identity(void)
{
    kernel_user_t** slot = current_user_slot();
    kernel_user_t* previous = *slot;
    *slot = &users[0];
    int result = kernel_user_login("test", "test-password");
    *slot = previous;
    return result == 0 && *slot == previous;
}
#endif

int kernel_user_verify_current_password(const char* password)
{
    kernel_user_t* user = kernel_user_current();
    int index = -1;

    if (!user || !valid_password(password) || user->locked)
        return 0;
    for (int i = 0; i < user_count; i++) {
        if (&users[i] == user) {
            index = i;
            break;
        }
    }
    if (index < 0 || user_disabled[index])
        return 0;
    return verify_password(password, user_salts[index], user_hashes[index]);
}

void kernel_user_logout(void)
{
    int console = tty_execution_console();
    if (console < 0 || console >= TTY_CONSOLE_COUNT) console = 0;
    *current_user_slot() = NULL;
    char selected[64];
    char path[96];
    uint32_t size = 0;
    if (!fs_load_file("/etc/keyboards/default", (uint8_t*)selected,
                      sizeof(selected) - 1, &size) ||
        size >= sizeof(selected))
        return;
    selected[size] = 0;
    while (size && (selected[size - 1] == '\n' || selected[size - 1] == '\r'))
        selected[--size] = 0;
    if (!keyboard_layout_name_valid(selected, size) ||
        (strcmp(selected, "us.lyt") != 0 &&
         strcmp(selected, "fr-azerty.lyt") != 0))
        return;
    strcpy(path, "/etc/keyboards/");
    strcat(path, selected);
    (void)keyboard_load_layout_for_console(path, console);
}

kernel_user_t* kernel_user_current(void) { return *current_user_slot(); }

int kernel_user_setpass(const char* name, const char* password)
{
    kernel_user_t* user;
    if (!valid_password(password) || !kernel_user_find(name, &user)) return 0;
    int index = (int)(user - users);
    uint8_t new_salt[sizeof(user_salts[index])];
    uint8_t previous_salt[sizeof(user_salts[index])];
    uint8_t previous_hash[sizeof(user_hashes[index])];
    if (!generate_salt(new_salt)) {
        serial_puts("[users] ERROR: secure random source unavailable\n");
        return 0;
    }
    memcpy(previous_salt, user_salts[index], sizeof(previous_salt));
    memcpy(previous_hash, user_hashes[index], sizeof(previous_hash));
    memcpy(user_salts[index], new_salt, sizeof(new_salt));
    hash_password(password, user_salts[index], user_hashes[index]);
    if (!save_users()) {
        memcpy(user_salts[index], previous_salt, sizeof(previous_salt));
        memcpy(user_hashes[index], previous_hash, sizeof(previous_hash));
        return 0;
    }
    return 1;
}

int kernel_user_set_locked(const char* name, int locked)
{
    kernel_user_t* user;
    if (!kernel_user_find(name, &user)) return 0;
    int previous = user->locked;
    user->locked = locked != 0;
    if (!save_users()) {
        user->locked = previous;
        return 0;
    }
    return 1;
}

int kernel_user_set_disabled(const char* name, int disabled)
{
    kernel_user_t* user;
    if (!kernel_user_find(name, &user)) return 0;
    int index = (int)(user - users);
    uint8_t previous = user_disabled[index];
    user_disabled[index] = disabled != 0;
    if (!save_users()) {
        user_disabled[index] = previous;
        return 0;
    }
    return 1;
}

int kernel_user_set_role(const char* name, int role)
{
    kernel_user_t* user;
    if (role < 0 || role > 3 || !kernel_user_find(name, &user))
        return 0;
    int previous = user->role;
    user->role = role;
    if (!save_users()) {
        user->role = previous;
        return 0;
    }
    return 1;
}