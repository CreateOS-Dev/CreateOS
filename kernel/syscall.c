// ============================================================
// CreateOS 0.0.1
// ============================================================

#include "syscall.h"
#include "drivers/vga.h"
#include "drivers/timer.h"
#include "drivers/serial.h"
#include "fs/fs.h"
#include "fs/cfs5.h"
#include "memory/pmm.h"
#include "memory/vmm.h"
#include "lib/string.h"
#include "proc/proc.h"
#include "proc/appl.h"
#include "proc/user.h"
#include "security/user_access.h"
#include "security/process.h"
#include "drivers/rtc.h"
#include "drivers/tty.h"
#include "drivers/keyboard.h"
#include "drivers/audio.h"
#include "drivers/usb.h"
#include "drivers/framebuffer.h"
#include "drivers/acpi.h"
#include "drivers/block.h"
#include "drivers/hpet.h"
#include "drivers/pci.h"
#include "arch/x86_64/random.h"
#include "drivers/netdev.h"
#include "drivers/netstack.h"
#include "version.h"

#define MAX_SYSCALLS 128
#define MIRROR_USER_OUTPUT_TO_SERIAL 0
#define SHELL_HISTORY_COUNT 64
#define SHELL_HISTORY_LENGTH 256
#define POSIX_FD_FIRST 3
#define POSIX_FD_COUNT 32
#define POSIX_FD_SLOT_BITS 5
#define POSIX_FD_SLOT_MASK (POSIX_FD_COUNT - 1)
_Static_assert(POSIX_FD_COUNT == (1U << POSIX_FD_SLOT_BITS),
               "POSIX descriptor token layout changed");
#define POSIX_EBADF 9
#define POSIX_ECHILD 10
#define POSIX_EAGAIN 11
#define POSIX_EACCES 13
#define POSIX_EEXIST 17
#define POSIX_EFAULT 14
#define POSIX_EINVAL 22
#define POSIX_EMFILE 24
#define POSIX_ENOENT 2
#define POSIX_ENOTDIR 20
#define POSIX_EISDIR 21
#define POSIX_ESPIPE 29
#define POSIX_EIO 5
#define POSIX_ENOMEM 12
#define POSIX_ENOSPC 28
#define POSIX_O_ACCMODE 3
#define POSIX_O_CREAT 0x0040
#define POSIX_O_EXCL 0x0080
#define POSIX_O_TRUNC 0x0200
#define POSIX_O_APPEND 0x0400
#define SYSCALL_RUN_MAX_DEPTH 8
#define SYSCALL_FORK_CHILD_MAX 4
#define SYSCALL_FRAME_QWORDS 15
#define SYSCALL_PROCESS_RETURN 0xC0DEC0DE

typedef struct {
    uint8_t used;
    uint8_t access_mode;
    uint8_t append;
    uint8_t directory;
    uint32_t offset;
    uint32_t directory_offset;
    uint64_t owner_generation;
    char path[256];
} posix_fd_t;

typedef struct {
    uint64_t syscall_frame[SYSCALL_FRAME_QWORDS];
    uint64_t user_rsp;
    char cwd[256];
    uint16_t file_creation_umask;
    uint64_t wait_status_pointer;
    int parent_pid;
    int child_pid;
    int fork_slot;
    uint8_t is_wait;
} syscall_run_context_t;

typedef struct {
    uint8_t used;
    int pid;
    int parent_pid;
    uint16_t file_creation_umask;
    char cwd[256];
    uint64_t syscall_frame[SYSCALL_FRAME_QWORDS];
    uint64_t user_rsp;
    appl_user_fork_state_t user_state;
} syscall_fork_child_t;

static syscall_entry_t syscall_table[MAX_SYSCALLS];
static uint32_t syscall_count = 0;
static char syscall_cursor_sprite[4096];
static char shell_history[TTY_CONSOLE_COUNT][SHELL_HISTORY_COUNT]
                        [SHELL_HISTORY_LENGTH];
static uint32_t shell_history_count[TTY_CONSOLE_COUNT];
static posix_fd_t posix_fds[TTY_CONSOLE_COUNT][POSIX_FD_COUNT];
static uint8_t posix_stdio_closed[TTY_CONSOLE_COUNT];
static uint8_t return_to_explorer[TTY_CONSOLE_COUNT];
static uint16_t syscall_file_creation_umask[TTY_CONSOLE_COUNT];
static int syscall_file_creation_umask_initialized;
static uint8_t net_tcp_request_buffer[NET_TCP_MAX_REQUEST];
static uint8_t net_tcp_response_buffer[NET_TCP_MAX_RESPONSE];
static uint8_t net_tcp_stream_buffer[NET_TCP_STREAM_IO_MAX];
static int net_tcp_syscall_busy;
static int net_dns_syscall_busy;
static int net_tcp_stream_owner_console = -1;
static uint64_t net_tcp_stream_owner_generation;
static int net_tcp_stream_id;
static int net_tcp_listener_owner_console = -1;
static uint64_t net_tcp_listener_owner_generation;
static int net_tcp_listener_id;
static uint8_t system_clipboard[4096];
static size_t system_clipboard_length;
static syscall_run_context_t syscall_run_contexts[SYSCALL_RUN_MAX_DEPTH];
static syscall_fork_child_t syscall_fork_children[SYSCALL_FORK_CHILD_MAX];
static int syscall_current_pid = 1;
static int syscall_next_pid = 2;
volatile uint64_t pending_process_frame;
volatile uint64_t pending_process_user_rsp;
volatile uint64_t pending_process_return_value;
extern uint64_t syscall_frame_pointer;
extern uint64_t current_rsp_saved;

static void syscall_reap_fork_children(int parent_pid)
{
    for (int i = 0; i < SYSCALL_FORK_CHILD_MAX; i++) {
        syscall_fork_child_t* child = &syscall_fork_children[i];
        if (!child->used || child->parent_pid != parent_pid)
            continue;
        if (child->user_state.address_space.pml4)
            vmm_address_space_destroy(&child->user_state.address_space);
        memset(child, 0, sizeof(*child));
    }
}

static int syscall_cursor_theme_supported(const uint8_t* contents,
                                          size_t length)
{
    static const char* const themes[] = {
        "classic.srbmp",
        "high-contrast.srbmp",
        "inverted.srbmp",
        "createos.srbmp",
        "blue-arrow.srbmp",
        "red-arrow.srbmp",
        "crosshair.srbmp",
        "precision.srbmp",
        "hand.srbmp"
    };
    for (size_t i = 0; i < sizeof(themes) / sizeof(themes[0]); i++) {
        size_t theme_length = strlen(themes[i]);
        if (length == theme_length &&
            memcmp(contents, themes[i], theme_length) == 0)
            return 1;
    }
    return 0;
}

static int syscall_cursor_pointer_mode(int mode)
{
    char path[112] = "/usr/share/createos/cursors/";
    uint32_t sprite_size = 0;
    size_t prefix_length = sizeof("/usr/share/createos/cursors/") - 1;
    if (mode == 1) {
        memcpy(path + prefix_length, "hand.srbmp", sizeof("hand.srbmp"));
    } else if (mode == 2) {
        memcpy(path + prefix_length, "text.srbmp", sizeof("text.srbmp"));
    } else if (mode == 0) {
        char name[64];
        uint32_t name_size = 0;
        if (!fs_load_file("/etc/cursors/default", (uint8_t*)name,
                          sizeof(name) - 1, &name_size) ||
            name_size >= sizeof(name))
            return -1;
        name[name_size] = 0;
        while (name_size &&
               (name[name_size - 1] == '\n' || name[name_size - 1] == '\r'))
            name[--name_size] = 0;
        if (!syscall_cursor_theme_supported((const uint8_t*)name, name_size))
            return -1;
        memcpy(path + prefix_length, name, name_size + 1);
    } else {
        return -1;
    }
    if (!framebuffer_ready() ||
        !fs_load_file(path, (uint8_t*)syscall_cursor_sprite,
                      sizeof(syscall_cursor_sprite), &sprite_size) ||
        !framebuffer_set_pointer_sprite(syscall_cursor_sprite, sprite_size))
        return -1;
    return 0;
}

static void syscall_exit_halt(const char* reason) __attribute__((noreturn));
static int64_t syscall_finish_user_child(int status);

static int syscall_current_console(void)
{
    int console = tty_execution_console();
    return console >= 0 && console < TTY_CONSOLE_COUNT ? console : 0;
}

static int syscall_current_admin_allowed(void)
{
    return kernel_user_is_admin() &&
           appl_current_has_system_privileges();
}

static int syscall_network_admin_allowed(void)
{
    return kernel_user_current() && syscall_current_admin_allowed();
}

static int syscall_network_admin_or_bootstrap_allowed(void)
{
    kernel_user_t* user = kernel_user_current();
    return !user || syscall_current_admin_allowed();
}

static posix_fd_t* syscall_fd_lookup(uint64_t descriptor)
{
    uint32_t index;
    uint64_t token;
    uint64_t generation;
    int console = syscall_current_console();
    if (descriptor < POSIX_FD_FIRST || descriptor > INT32_MAX)
        return NULL;
    token = descriptor - POSIX_FD_FIRST;
    generation = token >> POSIX_FD_SLOT_BITS;
    index = (uint32_t)(token & POSIX_FD_SLOT_MASK);
    if (index >= POSIX_FD_COUNT ||
        !security_process_generation_matches(console, generation))
        return NULL;
    if (!posix_fds[console][index].used ||
        !security_process_generation_matches(
            console, posix_fds[console][index].owner_generation))
        return NULL;
    return &posix_fds[console][index];
}

static uint64_t syscall_fd_handle(uint64_t generation, uint32_t index)
{
    if (!generation || generation > SECURITY_PROCESS_GENERATION_MAX ||
        index >= POSIX_FD_COUNT)
        return 0;
    uint64_t handle =
        ((generation << POSIX_FD_SLOT_BITS) | index) + POSIX_FD_FIRST;
    return handle < APPL_USER_IMAGE_BASE ? handle : 0;
}

static void syscall_close_current_fds(void)
{
    int console = syscall_current_console();
    memset(posix_fds[console], 0,
           sizeof(posix_fds[0]));
    posix_stdio_closed[console] = 0;
}

void syscall_process_transition(void)
{
    int console = syscall_current_console();

    tty_set_ctrl_c_interrupt(1);
    if (!security_advance_process_generation(console)) {
        syscall_exit_halt("process generation exhausted");
    }
    syscall_close_current_fds();

    if (net_tcp_stream_owner_console == console &&
        net_tcp_stream_owner_generation &&
        net_tcp_stream_owner_generation !=
            security_process_generation(console)) {
        if (net_tcp_syscall_busy) {
            serial_puts("[security] ERROR: TCP stream still busy at process transition\n");
            return;
        }
        net_tcp_syscall_busy = 1;
        __asm__ volatile("sti" ::: "memory");
        int result = net_tcp_stream_close(net_tcp_stream_id);
        __asm__ volatile("cli" ::: "memory");
        net_tcp_syscall_busy = 0;
        if (result != 0)
            serial_puts("[security] ERROR: failed to close previous process TCP stream\n");
        net_tcp_stream_owner_console = -1;
        net_tcp_stream_owner_generation = 0;
        net_tcp_stream_id = 0;
    }
    if (net_tcp_listener_owner_console == console &&
        net_tcp_listener_owner_generation &&
        net_tcp_listener_owner_generation !=
            security_process_generation(console)) {
        if (net_tcp_syscall_busy) {
            serial_puts("[security] ERROR: TCP listener still busy at process transition\n");
            return;
        }
        net_tcp_syscall_busy = 1;
        __asm__ volatile("sti" ::: "memory");
        int result = net_tcp_listener_close(net_tcp_listener_id);
        __asm__ volatile("cli" ::: "memory");
        net_tcp_syscall_busy = 0;
        if (result != 0)
            serial_puts("[security] ERROR: failed to close previous process TCP listener\n");
        net_tcp_listener_owner_console = -1;
        net_tcp_listener_owner_generation = 0;
        net_tcp_listener_id = 0;
    }
}

#ifdef PROC_CONTEXT_SELFTEST
int syscall_test_process_handle_revocation(void)
{
    int console = syscall_current_console();
    uint64_t generation = security_process_generation(console);
    uint64_t old_handle = syscall_fd_handle(generation, 0);
    posix_fd_t* descriptor = &posix_fds[console][0];

    if (!old_handle ||
        syscall_fd_handle(SECURITY_PROCESS_GENERATION_MAX,
                          POSIX_FD_COUNT - 1) >= APPL_USER_IMAGE_BASE)
        return 0;
    memset(descriptor, 0, sizeof(*descriptor));
    descriptor->used = 1;
    descriptor->owner_generation = generation;
    if (syscall_fd_lookup(old_handle) != descriptor) {
        syscall_close_current_fds();
        return 0;
    }

    syscall_process_transition();
    if (descriptor->used ||
        security_process_generation_matches(console, generation) ||
        syscall_fd_lookup(old_handle) != NULL) {
        syscall_close_current_fds();
        return 0;
    }

    descriptor->used = 1;
    descriptor->owner_generation = security_process_generation(console);
    uint64_t new_handle = syscall_fd_handle(
        descriptor->owner_generation, 0);
    int stale_handle_rejected =
        new_handle != old_handle &&
        syscall_fd_lookup(old_handle) == NULL &&
        syscall_fd_lookup(new_handle) == descriptor;
    syscall_close_current_fds();
    return stale_handle_rejected;
}
#endif

static void syscall_clear_shell_history(int console)
{
    if (console < 0 || console >= TTY_CONSOLE_COUNT) return;
    memset(shell_history[console], 0, sizeof(shell_history[console]));
    shell_history_count[console] = 0;
}

/* Alignement fixe de 32 octets pour éviter les ruptures d'itération linker */
typedef struct {
    const char* name;
    syscall_handler_t handler;
    uint8_t num_args;
    uint32_t syscall_num;
    uint8_t reserved[11];
} __attribute__((aligned(32))) syscall_reg_t;

extern const syscall_reg_t __start_syscalls[];
extern const syscall_reg_t __stop_syscalls[];

static void syscall_exit_halt(const char* reason)
{
    serial_puts("FATAL: ");
    serial_puts(reason);
    serial_puts("\n");
    tty_write("FATAL: unable to start a recovery shell; system halted\n", 0x0C);
    __asm__ volatile("cli");
    for (;;) __asm__ volatile("hlt");
}

static int syscall_path_is_within(const char* path, const char* directory);
static int syscall_path_is_public(const char* path);
static int syscall_normalize_path(const char* input, char* output,
                                  size_t output_size);

static int syscall_firstboot_allowed(void)
{
    fs_stat_t marker;
    return kernel_user_count() == 0 &&
           !fs_stat("/etc/.firstboot_done", &marker);
}

static int syscall_check_path_access(const char* path, int want)
{
    kernel_user_t* user = kernel_user_current();
    if (!user) {
        if (!(want & FS_PERM_WRITE) &&
            !strcmp(path, "/etc/network.conf"))
            return fs_check_access(path, 1000, 1000, want);
        if ((want & FS_PERM_EXEC) &&
            (!strcmp(path, "/bin/login") || !strcmp(path, "/bin/sh") ||
             (!strcmp(path, "/bin/firstboot") &&
              syscall_firstboot_allowed())))
            return fs_check_access(path, 1000, 1000, want);
        return 0;
    }
    if (syscall_current_admin_allowed())
        return fs_check_access(path, 0, 0, want);

    int uid = kernel_user_uid(user);
    if (uid < 0)
        return 0;
    if (syscall_path_is_within(path, user->home))
        return fs_check_access(path, (uint16_t)uid, (uint16_t)uid, want);

    if (!(want & FS_PERM_WRITE) && syscall_path_is_public(path))
        return fs_check_access(path, 1000, 1000, want);

    return 0;
}

static int syscall_path_is_within(const char* path, const char* directory)
{
    size_t length;
    if (!path || !directory) return 0;
    length = strlen(directory);
    if (length == 0 || strncmp(path, directory, length) != 0) return 0;
    return path[length] == 0 || path[length] == '/' ||
           (length == 1 && directory[0] == '/');
}

static int syscall_assign_new_path_owner(const char* path)
{
    for (int i = 0; i < kernel_user_count(); i++) {
        kernel_user_t account;
        kernel_user_t* stored_account = NULL;
        if (!kernel_user_get(i, &account))
            return 0;
        if (!syscall_path_is_within(path, account.home))
            continue;
        if (!kernel_user_find(account.name, &stored_account) ||
            !stored_account)
            return 0;
        int uid = kernel_user_uid(stored_account);
        if (uid < 0)
            return 0;
        return fs_chown(path, (uint16_t)uid, (uint16_t)uid);
    }
    return 1;
}

static int syscall_path_is_public(const char* path)
{
    return !strcmp(path, "/etc/network.conf") ||
           !strcmp(path, "/etc/power.conf") ||
           syscall_path_is_within(path, "/etc/keyboards") ||
           syscall_path_is_within(path, "/etc/cursors") ||
           syscall_path_is_within(path, "/bin") ||
           syscall_path_is_within(path, "/usr/bin") ||
           syscall_path_is_within(path, "/app");
}

static int syscall_resolve_user_path(const char* input, char* output,
                                     size_t output_size, int allow_public)
{
    char combined[512];
    const char* cwd = tty_get_cwd();
    size_t cwd_length;
    size_t input_length;

    if (!input || !output || output_size < 2) return 0;
    if (input[0] == '/') {
        if (strlen(input) >= sizeof(combined)) return 0;
        strcpy(combined, input);
    } else {
        if (!cwd || cwd[0] != '/') return 0;
        cwd_length = strlen(cwd);
        input_length = strlen(input);
        if (cwd_length + input_length + 2 > sizeof(combined)) return 0;
        strcpy(combined, cwd);
        if (cwd_length > 1) strcat(combined, "/");
        strcat(combined, input);
    }

    if (!syscall_normalize_path(combined, output, output_size)) return 0;

    kernel_user_t* user = kernel_user_current();
    if (!user) {
        return !strcmp(output, "/bin/login") || !strcmp(output, "/bin/sh") ||
               (!strcmp(output, "/bin/firstboot") &&
                syscall_firstboot_allowed()) ||
               (allow_public &&
                (!strcmp(output, "/etc/network.conf") ||
                 !strcmp(output, "/etc/power.conf")));
    }
    if (syscall_current_admin_allowed()) return 1;
    if (syscall_path_is_within(output, user->home)) return 1;
    return allow_public && syscall_path_is_public(output);
}

static int syscall_resolve_parent_path(const char* path, char* parent,
                                       size_t parent_size)
{
    size_t length;
    char* slash;
    if (!path || !parent || parent_size < 2) return 0;
    length = strlen(path);
    if (length >= parent_size) return 0;
    memcpy(parent, path, length + 1);
    slash = strrchr(parent, '/');
    if (!slash) return 0;
    if (slash == parent) slash[1] = 0;
    else *slash = 0;
    return 1;
}

static int syscall_check_parent_write_access(const char* path)
{
    char parent[256];
    if (!syscall_resolve_parent_path(path, parent, sizeof(parent))) return 0;
    return syscall_check_path_access(parent, FS_PERM_WRITE | FS_PERM_EXEC);
}

static int syscall_normalize_path(const char* input, char* output, size_t output_size)
{
    size_t out_length = 1;
    const char* cursor = input;
    if (!input || !output || output_size < 2 || input[0] != '/') return 0;
    output[0] = '/';

    while (*cursor) {
        size_t component_length;
        while (*cursor == '/') cursor++;
        if (!*cursor) break;
        const char* component = cursor;
        while (*cursor && *cursor != '/') cursor++;
        component_length = (size_t)(cursor - component);
        if (component_length == 1 && component[0] == '.') continue;
        if (component_length == 2 && component[0] == '.' && component[1] == '.') {
            if (out_length > 1) {
                while (out_length > 1 && output[out_length - 1] != '/') out_length--;
                if (out_length > 1) out_length--;
            }
            continue;
        }
        if (output_size - out_length < 2 ||
            component_length > output_size - out_length - 2)
            return 0;
        if (out_length > 1) output[out_length++] = '/';
        memcpy(output + out_length, component, component_length);
        out_length += component_length;
    }
    output[out_length] = 0;
    return 1;
}

/* Macro de enregistrement des syscalls */
#define REGISTER_SYSCALL(sys_num, sys_name, nargs) \
    static const syscall_reg_t __syscall_reg_##sys_name \
        __attribute__((section(".syscalls"), used)) = { \
            .name = #sys_name, \
            .handler = (syscall_handler_t)&sys_name, \
            .num_args = nargs, \
            .syscall_num = sys_num \
        };

/* ============================================================
 * SYSCALL IMPLEMENTATIONS
 * ============================================================ */

static int64_t sys_exit(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    if (a1 == 0xDEAD || a1 == 0xBEEF) {
        if (!syscall_current_admin_allowed())
            return -POSIX_EACCES;
        if (!vfs_sync()) {
            serial_puts("[syscall] refusing reboot/shutdown: filesystem sync failed\n");
            return -1;
        }
        if (a1 == 0xDEAD) acpi_reboot();
    }
    if (a1 == 0xBEEF) {
        __asm__ volatile("cli");
        for (;;) __asm__ volatile("hlt");
    }

    int child_depth = appl_user_child_depth();
    if (child_depth) {
        if (child_depth <= SYSCALL_RUN_MAX_DEPTH &&
            syscall_run_contexts[child_depth - 1].is_wait)
            syscall_reap_fork_children(syscall_current_pid);
        return syscall_finish_user_child((int)(int32_t)a1);
    }

    syscall_reap_fork_children(syscall_current_pid);
    serial_puts("[syscall] exit() - returning to shell\n");
    syscall_close_current_fds();
    tty_fullscreen_end();
    tty_newline_if_needed();
    if (a1 == 130)
        tty_write("^C\n", 0x0C);
    int console = syscall_current_console();
    if (return_to_explorer[console]) {
        return_to_explorer[console] = 0;
        int rc = user_exec_from_user("/app/system/bin/explorer.APPL", NULL);
        if ((uint32_t)rc == 0xDEADBEEF)
            return (int64_t)0xDEADBEEF;
        serial_puts("[syscall] Explorer recovery failed; returning to shell\n");
    }
    /* Exit must hand control back to the shell via the exec-return path.
     * Returning normally resumes user code, which reaches the privileged
     * cli/hlt instructions in user_command_start and triggers a #GP. */
    {
        int rc = user_exec_from_user("/bin/sh", NULL);
        if ((uint32_t)rc != 0xDEADBEEF) {
            serial_puts("[syscall] Shell restart failed; trying login recovery\n");
            rc = user_exec_from_user("/bin/login", NULL);
        }
        if ((uint32_t)rc != 0xDEADBEEF)
            syscall_exit_halt("shell and login recovery failed");
        return (int64_t)0xDEADBEEF;
    }
}
REGISTER_SYSCALL(SYS_EXIT, sys_exit, 1)

static int64_t sys_putc(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    tty_putchar_user((char)a1, (uint8_t)a2);
#if MIRROR_USER_OUTPUT_TO_SERIAL
    serial_putc((char)a1);
#endif
    return 0;
}
REGISTER_SYSCALL(SYS_PUTC, sys_putc, 2)

static int64_t sys_puts(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    if (!security_valid_user_string((const char*)a1, 4096)) return -1;
    tty_write_user((const char*)a1, (uint8_t)a2);
#if MIRROR_USER_OUTPUT_TO_SERIAL
    serial_puts((const char*)a1);
#endif
    return 0;
}
REGISTER_SYSCALL(SYS_PUTS, sys_puts, 2)

static int64_t sys_getc(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    int key = (unsigned char)tty_getchar();
    if (key == KEY_INTERRUPT)
        return sys_exit(130, 0, 0, 0, 0, 0);
    return key;
}
REGISTER_SYSCALL(SYS_GETC, sys_getc, 0)

static int64_t sys_putchar(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
#if MIRROR_USER_OUTPUT_TO_SERIAL
    serial_putc((char)a1);
#endif
    tty_putchar_user((char)a1, 0x0F);
    return 0;
}
REGISTER_SYSCALL(SYS_PUTCHAR, sys_putchar, 1)

static int64_t sys_clear(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    tty_clear();
    return 0;
}
REGISTER_SYSCALL(SYS_CLEAR, sys_clear, 0)

static int64_t sys_tty_set_cursor(uint64_t a1, uint64_t a2, uint64_t a3,
                                  uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3; (void)a4; (void)a5; (void)a6;
    if (a1 > INT32_MAX || a2 > INT32_MAX) return -1;
    return tty_set_cursor((int)a1, (int)a2);
}
REGISTER_SYSCALL(SYS_TTY_SET_CURSOR, sys_tty_set_cursor, 2)

static int64_t sys_tty_fullscreen_begin(uint64_t a1, uint64_t a2,
                                        uint64_t a3, uint64_t a4,
                                        uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    tty_fullscreen_begin();
    return 0;
}
REGISTER_SYSCALL(SYS_TTY_FULLSCREEN_BEGIN, sys_tty_fullscreen_begin, 0)

static int64_t sys_tty_fullscreen_end(uint64_t a1, uint64_t a2,
                                      uint64_t a3, uint64_t a4,
                                      uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    tty_fullscreen_end();
    return 0;
}
REGISTER_SYSCALL(SYS_TTY_FULLSCREEN_END, sys_tty_fullscreen_end, 0)

static int64_t sys_tty_mouse_position(uint64_t a1, uint64_t a2,
                                      uint64_t a3, uint64_t a4,
                                      uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    return (int64_t)tty_mouse_position();
}
REGISTER_SYSCALL(SYS_TTY_MOUSE_POSITION, sys_tty_mouse_position, 0)

static int64_t sys_tty_pointer_mode(uint64_t a1, uint64_t a2,
                                    uint64_t a3, uint64_t a4,
                                    uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    if (a1 > 2)
        return -1;
    return syscall_cursor_pointer_mode((int)a1);
}
REGISTER_SYSCALL(SYS_TTY_POINTER_MODE, sys_tty_pointer_mode, 1)

static int64_t sys_tty_pollchar(uint64_t a1, uint64_t a2,
                                uint64_t a3, uint64_t a4,
                                uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    int key = (unsigned char)tty_pollchar();
    if (key == KEY_INTERRUPT)
        return sys_exit(130, 0, 0, 0, 0, 0);
    return key;
}
REGISTER_SYSCALL(SYS_TTY_POLLCHAR, sys_tty_pollchar, 0)

static int64_t sys_tty_cursor_position(uint64_t a1, uint64_t a2,
                                       uint64_t a3, uint64_t a4,
                                       uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    return tty_cursor_position();
}
REGISTER_SYSCALL(SYS_TTY_CURSOR_POSITION, sys_tty_cursor_position, 0)

static int64_t sys_clipboard_set(uint64_t a1, uint64_t a2,
                                 uint64_t a3, uint64_t a4,
                                 uint64_t a5, uint64_t a6)
{
    (void)a3; (void)a4; (void)a5; (void)a6;
    if (a2 > sizeof(system_clipboard) ||
        (a2 && !security_copy_from_user(system_clipboard,
                                        (const void*)a1, (size_t)a2)))
        return -1;
    system_clipboard_length = (size_t)a2;
    return 0;
}
REGISTER_SYSCALL(SYS_CLIPBOARD_SET, sys_clipboard_set, 2)

static int64_t sys_clipboard_get(uint64_t a1, uint64_t a2,
                                 uint64_t a3, uint64_t a4,
                                 uint64_t a5, uint64_t a6)
{
    size_t count;
    (void)a3; (void)a4; (void)a5; (void)a6;
    if (a2 > sizeof(system_clipboard))
        return -1;
    count = system_clipboard_length < (size_t)a2
        ? system_clipboard_length : (size_t)a2;
    if (count && !security_copy_to_user((void*)a1, system_clipboard, count))
        return -1;
    return (int64_t)system_clipboard_length;
}
REGISTER_SYSCALL(SYS_CLIPBOARD_GET, sys_clipboard_get, 2)

static int64_t sys_tty_shell_input(uint64_t a1, uint64_t a2,
                                   uint64_t a3, uint64_t a4,
                                   uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    if (a1 > 1)
        return -1;
    tty_shell_input((int)a1);
    return 0;
}
REGISTER_SYSCALL(SYS_TTY_SHELL_INPUT, sys_tty_shell_input, 1)

static int64_t sys_tty_ctrl_c_interrupt(uint64_t a1, uint64_t a2,
                                        uint64_t a3, uint64_t a4,
                                        uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    if (a1 > 1)
        return -POSIX_EINVAL;
    tty_set_ctrl_c_interrupt((int)a1);
    return 0;
}
REGISTER_SYSCALL(SYS_TTY_CTRL_C_INTERRUPT, sys_tty_ctrl_c_interrupt, 1)

static int64_t sys_app_return_explorer(uint64_t a1, uint64_t a2,
                                       uint64_t a3, uint64_t a4,
                                       uint64_t a5, uint64_t a6)
{
    int console = syscall_current_console();
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    if (!a1) {
        return_to_explorer[console] = 0;
        return 0;
    }
    if (!kernel_user_current() ||
        !syscall_check_path_access("/app/system/bin/explorer.APPL",
                                   FS_PERM_READ | FS_PERM_EXEC) ||
        !syscall_check_path_access(tty_get_cwd(),
                                   FS_PERM_READ | FS_PERM_EXEC))
        return -1;
    return_to_explorer[console] = 1;
    return 0;
}
REGISTER_SYSCALL(SYS_APP_RETURN_EXPLORER, sys_app_return_explorer, 1)

static int64_t sys_beep(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    audio_beep((uint32_t)a1, (uint32_t)a2);
    return 0;
}
REGISTER_SYSCALL(SYS_BEEP, sys_beep, 2)

static int64_t sys_font_set(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    return framebuffer_set_font((int)a1) ? 0 : -1;
}
REGISTER_SYSCALL(SYS_FONT_SET, sys_font_set, 1)

static int64_t sys_font_get(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    return framebuffer_font();
}
REGISTER_SYSCALL(SYS_FONT_GET, sys_font_get, 0)

static int64_t sys_font_count(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    return framebuffer_font_count();
}
REGISTER_SYSCALL(SYS_FONT_COUNT, sys_font_count, 0)

static int64_t sys_open(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    char input_path[256];
    char resolved[256];
    fs_stat_t st;
    uint32_t flags = (uint32_t)a2;
    int access_mode = (int)(flags & POSIX_O_ACCMODE);
    int exists;
    int created = 0;
    int want;
    uint32_t fd_index;

    if (a2 > UINT32_MAX || a3 > UINT32_MAX ||
        access_mode == POSIX_O_ACCMODE ||
        (flags & ~(uint32_t)(POSIX_O_ACCMODE | POSIX_O_CREAT |
                             POSIX_O_EXCL |
                             POSIX_O_TRUNC | POSIX_O_APPEND)) ||
        ((flags & POSIX_O_EXCL) && !(flags & POSIX_O_CREAT)) ||
        ((flags & POSIX_O_TRUNC) && access_mode == 0))
        return -POSIX_EINVAL;
    if (!security_copy_user_string(input_path, sizeof(input_path),
                                   (const char*)a1, sizeof(input_path)))
        return -POSIX_EFAULT;
    if (!syscall_resolve_user_path(input_path, resolved,
                                   sizeof(resolved), 1))
        return -POSIX_EACCES;

    for (fd_index = 0; fd_index < POSIX_FD_COUNT; fd_index++) {
        posix_fd_t* descriptor =
            &posix_fds[syscall_current_console()][fd_index];
        uint64_t generation =
            security_process_generation(syscall_current_console());
        if (!descriptor->used ||
            descriptor->owner_generation != generation)
            break;
    }
    if (fd_index == POSIX_FD_COUNT) return -POSIX_EMFILE;

    exists = fs_stat(resolved, &st) > 0;
    if (exists && (flags & (POSIX_O_CREAT | POSIX_O_EXCL)) ==
                      (POSIX_O_CREAT | POSIX_O_EXCL))
        return -POSIX_EEXIST;
    if (!exists) {
        if (!(flags & POSIX_O_CREAT))
            return -POSIX_ENOENT;
        if (!syscall_check_parent_write_access(resolved))
            return -POSIX_EACCES;
        uint16_t create_mode = (uint16_t)a3 & 0777;
        create_mode &= (uint16_t)~syscall_file_creation_umask[
            syscall_current_console()];
        if (!fs_create(resolved, create_mode))
            return -POSIX_ENOSPC;
        if (!syscall_assign_new_path_owner(resolved)) {
            fs_unlink(resolved);
            return -POSIX_EIO;
        }
        if (!fs_stat(resolved, &st)) {
            fs_unlink(resolved);
            return -POSIX_EIO;
        }
        created = 1;
    }

    if (st.type != FS_TYPE_FILE && st.type != FS_TYPE_DIR)
        return -POSIX_EINVAL;
    if (st.type == FS_TYPE_DIR &&
        (access_mode != 0 || (flags & POSIX_O_TRUNC)))
        return -POSIX_EISDIR;

    want = access_mode == 2 ? (FS_PERM_READ | FS_PERM_WRITE) :
           access_mode == 1 ? FS_PERM_WRITE : FS_PERM_READ;
    if (!created && !syscall_check_path_access(resolved, want))
        return -POSIX_EACCES;

    if (flags & POSIX_O_TRUNC) {
        if (access_mode == 0 ||
            (!created &&
             !syscall_check_path_access(resolved, FS_PERM_WRITE)))
            return -POSIX_EACCES;
        if (!fs_write_file(resolved, (const uint8_t*)"", 0))
            return -POSIX_EIO;
    }

    posix_fd_t* descriptor =
        &posix_fds[syscall_current_console()][fd_index];
    memset(descriptor, 0, sizeof(*descriptor));
    descriptor->used = 1;
    descriptor->owner_generation =
        security_process_generation(syscall_current_console());
    descriptor->access_mode = (uint8_t)access_mode;
    descriptor->append = (flags & POSIX_O_APPEND) != 0;
    descriptor->directory = st.type == FS_TYPE_DIR;
    strncpy(descriptor->path, resolved, sizeof(descriptor->path) - 1);
    uint64_t handle = syscall_fd_handle(
        descriptor->owner_generation, fd_index);
    if (!handle) {
        memset(descriptor, 0, sizeof(*descriptor));
        return -POSIX_EMFILE;
    }
    return (int64_t)handle;
}
REGISTER_SYSCALL(SYS_OPEN, sys_open, 3)

static int64_t sys_read(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    if (a1 < APPL_USER_IMAGE_BASE) {
        posix_fd_t* descriptor = NULL;
        if (a1 > 2) {
            descriptor = syscall_fd_lookup(a1);
            if (!descriptor) return -POSIX_EBADF;
            if (descriptor->access_mode == 1) return -POSIX_EBADF;
            if (descriptor->directory) return -POSIX_EISDIR;
        } else if (posix_stdio_closed[syscall_current_console()] &
                   (1U << a1)) {
            return -POSIX_EBADF;
        }
        if (a3 > UINT32_MAX) return -POSIX_EINVAL;
        if (!a3) return 0;
        if (!security_valid_user_write_range((const void*)a2, (size_t)a3))
            return -POSIX_EFAULT;
        if (a1 == 0) {
            uint8_t* buffer = (uint8_t*)a2;
            uint32_t count = 0;
            while (count < (uint32_t)a3) {
                char input = tty_getchar();
                buffer[count++] = (uint8_t)input;
                if (input == '\n') break;
            }
            return count;
        }
        if (a1 == 1 || a1 == 2)
            return -POSIX_EBADF;
        fs_stat_t status;
        uint32_t count;
        if (!fs_stat(descriptor->path, &status) ||
            status.type != FS_TYPE_FILE)
            return -POSIX_EIO;
        if (descriptor->offset >= status.size) return 0;
        count = (uint32_t)a3;
        if (count > status.size - descriptor->offset)
            count = status.size - descriptor->offset;
        if (!count) return 0;
        if (!fs_read(descriptor->path, (uint8_t*)a2, count,
                     descriptor->offset))
            return -POSIX_EIO;
        descriptor->offset += count;
        return count;
    }

    char kpath[256];
    char resolved[256];
    if (a3 > UINT32_MAX ||
        !security_copy_user_string(kpath, sizeof(kpath),
                                   (const char*)a1, sizeof(kpath)) ||
        !security_valid_user_write_range((const void*)a2, (size_t)a3) ||
        !syscall_resolve_user_path(kpath, resolved, sizeof(resolved), 1))
        return -POSIX_EFAULT;

    if (!syscall_check_path_access(resolved, FS_PERM_READ))
        return -POSIX_EACCES;
    return fs_read(resolved, (uint8_t*)a2, (uint32_t)a3, 0);
}
REGISTER_SYSCALL(SYS_READ, sys_read, 3)

static int syscall_zero_file_range(const char* path, uint32_t start,
                                   uint32_t end)
{
    uint8_t zeros[256] = { 0 };
    while (start < end) {
        uint32_t count = end - start;
        if (count > sizeof(zeros)) count = sizeof(zeros);
        if (!fs_write(path, zeros, count, start))
            return 0;
        start += count;
    }
    return 1;
}

static int64_t sys_write(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    if (a1 < APPL_USER_IMAGE_BASE) {
        posix_fd_t* descriptor = NULL;
        if (a1 > 2) {
            descriptor = syscall_fd_lookup(a1);
            if (!descriptor) return -POSIX_EBADF;
            if (descriptor->access_mode == 0) return -POSIX_EBADF;
            if (descriptor->directory) return -POSIX_EISDIR;
        } else if (posix_stdio_closed[syscall_current_console()] &
                   (1U << a1)) {
            return -POSIX_EBADF;
        }
        if (a3 > UINT32_MAX) return -POSIX_EINVAL;
        if (!a3) return 0;
        if (!security_valid_user_range((const void*)a2, (size_t)a3))
            return -POSIX_EFAULT;
        if (a1 == 1 || a1 == 2) {
            const char* text = (const char*)a2;
            for (uint32_t i = 0; i < (uint32_t)a3; i++)
                tty_putchar(text[i], 0x0F);
            return (int64_t)a3;
        }
        if (a1 == 0)
            return -POSIX_EBADF;
        fs_stat_t status;
        uint32_t offset;
        if (!fs_stat(descriptor->path, &status) ||
            status.type != FS_TYPE_FILE)
            return -POSIX_EIO;
        offset = descriptor->append ? status.size : descriptor->offset;
        if (a3 > UINT32_MAX - offset) return -POSIX_EINVAL;
        if (offset > status.size &&
            !syscall_zero_file_range(descriptor->path, status.size, offset))
            return -POSIX_EIO;
        if (!fs_write(descriptor->path, (const uint8_t*)a2,
                      (uint32_t)a3, offset))
            return -POSIX_EIO;
        descriptor->offset = offset + (uint32_t)a3;
        return (int64_t)a3;
    }

    char kpath[256];
    char resolved[256];
    if (a3 > UINT32_MAX ||
        !security_copy_user_string(kpath, sizeof(kpath),
                                   (const char*)a1, sizeof(kpath)) ||
        !security_valid_user_range((const void*)a2, (size_t)a3) ||
        !syscall_resolve_user_path(kpath, resolved, sizeof(resolved), 0) ||
        !syscall_check_path_access(resolved, FS_PERM_WRITE))
        return -POSIX_EACCES;

    return fs_write(resolved, (const uint8_t*)a2, (uint32_t)a3, 0);
}
REGISTER_SYSCALL(SYS_WRITE, sys_write, 3)

static int64_t sys_file_replace(uint64_t a1, uint64_t a2, uint64_t a3,
                                uint64_t a4, uint64_t a5, uint64_t a6)
{
    char input_path[256];
    char resolved_path[256];
    uint8_t contents[4096];
    fs_stat_t status;
    int path_existed;
    char keyboard_layout_path[96];
    int updates_keyboard_layout = 0;
    char cursor_theme_path[112];
    int updates_cursor_theme = 0;
    char user_keyboard_layout[192];
    int updates_user_keyboard_layout = 0;
    (void)a4; (void)a5; (void)a6;

    if (a3 > sizeof(contents) ||
        !security_copy_user_string(input_path, sizeof(input_path),
                                   (const char*)a1, sizeof(input_path)) ||
        (a3 && !security_copy_from_user(contents, (const void*)a2,
                                        (size_t)a3)) ||
        !syscall_resolve_user_path(input_path, resolved_path,
                                   sizeof(resolved_path), 0))
        return -1;

    path_existed = fs_stat(resolved_path, &status);
    if (path_existed) {
        if (status.type != CFS5_INODE_FILE ||
            !syscall_check_path_access(resolved_path, FS_PERM_WRITE))
            return -1;
    } else if (!syscall_check_parent_write_access(resolved_path)) {
        return -1;
    }

    if (strcmp(resolved_path, "/etc/keyboards/default") == 0) {
        size_t length = (size_t)a3;
        if (length >= 64)
            return -1;
        while (length &&
               (contents[length - 1] == '\n' || contents[length - 1] == '\r'))
            length--;
        if ((length != 6 || memcmp(contents, "us.lyt", 6) != 0) &&
            (length != 13 ||
             memcmp(contents, "fr-azerty.lyt", 13) != 0))
            return -1;
        memcpy(keyboard_layout_path, "/etc/keyboards/",
               sizeof("/etc/keyboards/") - 1);
        memcpy(keyboard_layout_path + sizeof("/etc/keyboards/") - 1,
               contents, length);
        keyboard_layout_path[sizeof("/etc/keyboards/") - 1 + length] = 0;
        updates_keyboard_layout = 1;
    }
    if (strcmp(resolved_path, "/etc/cursors/default") == 0) {
        size_t length = (size_t)a3;
        if (!syscall_cursor_theme_supported(contents, length))
            return -1;
        memcpy(cursor_theme_path, "/usr/share/createos/cursors/",
               sizeof("/usr/share/createos/cursors/") - 1);
        memcpy(cursor_theme_path + sizeof("/usr/share/createos/cursors/") - 1,
               contents, length);
        cursor_theme_path[sizeof("/usr/share/createos/cursors/") - 1 +
                          length] = 0;
        updates_cursor_theme = 1;
    }
    kernel_user_t* current_user = kernel_user_current();
    if (current_user) {
        char expected_selection[128];
        strcpy(expected_selection, current_user->home);
        strcat(expected_selection, "/.config/keyboards/default");
        if (strcmp(resolved_path, expected_selection) == 0) {
            size_t length = (size_t)a3;
            if (length >= 64)
                return -1;
            while (length &&
                   (contents[length - 1] == '\n' ||
                    contents[length - 1] == '\r'))
                length--;
            if (length < 5 || memcmp(contents + length - 4, ".lyt", 4) != 0)
                return -1;
            for (size_t i = 0; i < length - 4; i++) {
                char ch = (char)contents[i];
                if (!((ch >= 'a' && ch <= 'z') ||
                      (ch >= 'A' && ch <= 'Z') ||
                      (ch >= '0' && ch <= '9') || ch == '-' || ch == '_'))
                    return -1;
            }
            strcpy(user_keyboard_layout, current_user->home);
            strcat(user_keyboard_layout, "/.config/keyboards/");
            memcpy(user_keyboard_layout + strlen(user_keyboard_layout),
                   contents, length);
            user_keyboard_layout[strlen(current_user->home) +
                                 sizeof("/.config/keyboards/") - 1 +
                                 length] = 0;
            if (!keyboard_validate_layout(user_keyboard_layout)) {
                if (!((length == 6 &&
                       memcmp(contents, "us.lyt", length) == 0) ||
                      (length == 13 &&
                       memcmp(contents, "fr-azerty.lyt", length) == 0)))
                    return -1;
                strcpy(user_keyboard_layout, "/etc/keyboards/");
                memcpy(user_keyboard_layout + sizeof("/etc/keyboards/") - 1,
                       contents, length);
                user_keyboard_layout[sizeof("/etc/keyboards/") - 1 +
                                     length] = 0;
                if (!keyboard_validate_layout(user_keyboard_layout))
                    return -1;
            }
            updates_user_keyboard_layout = 1;
        }
    }

    if (!fs_write_file(resolved_path, contents, (uint32_t)a3)) {
        if (!path_existed)
            fs_unlink(resolved_path);
        return -1;
    }
    if (!path_existed && !syscall_assign_new_path_owner(resolved_path)) {
        fs_unlink(resolved_path);
        return -1;
    }

    if (updates_keyboard_layout) {
        if (!keyboard_load_layout(keyboard_layout_path))
            return -1;
    }
    if (updates_cursor_theme) {
        uint32_t sprite_size = 0;
        if (!fs_load_file(cursor_theme_path,
                          (uint8_t*)syscall_cursor_sprite,
                          sizeof(syscall_cursor_sprite), &sprite_size) ||
            !framebuffer_set_pointer_sprite(syscall_cursor_sprite,
                                             sprite_size))
            return -1;
    }
    if (updates_user_keyboard_layout) {
        if (!keyboard_load_layout_for_console(user_keyboard_layout,
                                              syscall_current_console()))
            return -1;
    }
    return 0;
}
REGISTER_SYSCALL(SYS_FILE_REPLACE, sys_file_replace, 3)

static int64_t sys_read_at(uint64_t a1, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6)
{
    char input_path[256];
    char resolved_path[256];
    fs_stat_t status;
    uint32_t count;
    (void)a5; (void)a6;

    if (a3 > UINT32_MAX || a4 > UINT32_MAX ||
        !security_copy_user_string(input_path, sizeof(input_path),
                                   (const char*)a1, sizeof(input_path)) ||
        !security_valid_user_write_range((void*)a2, (size_t)a3) ||
        !syscall_resolve_user_path(input_path, resolved_path,
                                   sizeof(resolved_path), 1) ||
        !syscall_check_path_access(resolved_path, FS_PERM_READ))
        return -1;

    if (!fs_stat(resolved_path, &status) || status.type != FS_TYPE_FILE)
        return -1;
    if (a4 >= status.size) return 0;
    count = (uint32_t)a3;
    if (count > status.size - (uint32_t)a4)
        count = status.size - (uint32_t)a4;
    if (!count) return 0;
    return fs_read(resolved_path, (uint8_t*)a2, count, (uint32_t)a4)
        ? (int64_t)count : -1;
}
REGISTER_SYSCALL(SYS_READ_AT, sys_read_at, 4)

static int64_t sys_close(uint64_t a1, uint64_t a2, uint64_t a3,
                         uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    if (a1 <= 2) {
        uint8_t bit = (uint8_t)(1U << a1);
        uint8_t* closed = &posix_stdio_closed[syscall_current_console()];
        if (*closed & bit) return -POSIX_EBADF;
        *closed |= bit;
        return 0;
    }
    if (a1 < POSIX_FD_FIRST) return -POSIX_EBADF;
    posix_fd_t* descriptor = syscall_fd_lookup(a1);
    if (!descriptor) return -POSIX_EBADF;
    memset(descriptor, 0, sizeof(*descriptor));
    return 0;
}
REGISTER_SYSCALL(SYS_CLOSE, sys_close, 1)

static int64_t sys_seek(uint64_t a1, uint64_t a2, uint64_t a3,
                        uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a4; (void)a5; (void)a6;
    posix_fd_t* descriptor = syscall_fd_lookup(a1);
    fs_stat_t status;
    int64_t base;
    int64_t offset = (int64_t)a2;
    int64_t result;

    if (!descriptor) return -POSIX_EBADF;
    if (descriptor->directory) return -POSIX_ESPIPE;
    if (!fs_stat(descriptor->path, &status) ||
        status.type != FS_TYPE_FILE)
        return -POSIX_EIO;
    if (a3 == 0) base = 0;
    else if (a3 == 1) base = descriptor->offset;
    else if (a3 == 2) base = status.size;
    else return -POSIX_EINVAL;
    if ((offset > 0 && base > INT64_MAX - offset) ||
        (offset < 0 && base < INT64_MIN - offset))
        return -POSIX_EINVAL;
    result = base + offset;
    if (result < 0 || result > UINT32_MAX)
        return -POSIX_EINVAL;
    descriptor->offset = (uint32_t)result;
    return result;
}
REGISTER_SYSCALL(SYS_SEEK, sys_seek, 3)

static int64_t sys_stat(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    extern int fs_stat(const char* path, fs_stat_t* st);
    
    char kernel_path[256];
    char resolved_path[256];
    fs_stat_t kst;

    if (!security_copy_user_string(kernel_path, sizeof(kernel_path),
                                   (const char*)a1, sizeof(kernel_path)) ||
        !security_valid_user_write_range((void*)a2, sizeof(fs_stat_t))) {
        return -POSIX_EFAULT;
    }
    if (!syscall_resolve_user_path(kernel_path, resolved_path,
                                   sizeof(resolved_path), 1))
        return -POSIX_EACCES;
    if (!syscall_check_path_access(resolved_path, FS_PERM_READ))
        return -POSIX_EACCES;

    int res = fs_stat(resolved_path, &kst);
    if (res <= 0)
        return -POSIX_ENOENT;
    if (!security_copy_to_user((void*)a2, &kst, sizeof(fs_stat_t)))
        return -POSIX_EFAULT;
    return 0;
}
REGISTER_SYSCALL(SYS_STAT, sys_stat, 2)

static int64_t sys_fstat(uint64_t a1, uint64_t a2, uint64_t a3,
                         uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3; (void)a4; (void)a5; (void)a6;
    posix_fd_t* descriptor = syscall_fd_lookup(a1);
    fs_stat_t status;
    const char* path;

    if (a1 > 2 && !descriptor) return -POSIX_EBADF;
    if (a1 == 0 || a1 == 1 || a1 == 2) {
        if (posix_stdio_closed[syscall_current_console()] &
            (1U << a1))
            return -POSIX_EBADF;
        memset(&status, 0, sizeof(status));
        status.type = FS_TYPE_FILE;
        status.mode = 0666;
    } else {
        path = descriptor->path;
        if (!fs_stat(path, &status))
            return -POSIX_EIO;
    }
    if (!security_valid_user_write_range((void*)a2, sizeof(status)))
        return -POSIX_EFAULT;
    return security_copy_to_user((void*)a2, &status, sizeof(status))
        ? 0 : -POSIX_EFAULT;
}
REGISTER_SYSCALL(SYS_FSTAT, sys_fstat, 2)

typedef struct __attribute__((packed)) {
    uint64_t inode;
    int64_t offset;
    uint16_t record_length;
    uint8_t type;
    char name[];
} posix_dirent64_t;

static int64_t sys_getdents64(uint64_t a1, uint64_t a2, uint64_t a3,
                              uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a4; (void)a5; (void)a6;
    posix_fd_t* descriptor = syscall_fd_lookup(a1);
    char names[32][CFS5_MAX_NAME];
    int count;
    uint32_t written = 0;

    if (!descriptor) return -POSIX_EBADF;
    if (!descriptor->directory) return -POSIX_ENOTDIR;
    if (descriptor->access_mode == 1) return -POSIX_EBADF;
    if (a3 > UINT32_MAX) return -POSIX_EINVAL;
    if (!a3) return -POSIX_EINVAL;
    if (!security_valid_user_write_range((void*)a2, (size_t)a3))
        return -POSIX_EFAULT;
    count = fs_list_offset(descriptor->path, names,
                           descriptor->directory_offset,
                           (int)(sizeof(names) / sizeof(names[0])));
    if (count < 0) return -POSIX_EIO;

    for (int i = 0; i < count; i++) {
        size_t name_length = strlen(names[i]);
        size_t header_size = offsetof(posix_dirent64_t, name);
        size_t record_length = (header_size + name_length + 1 + 7) & ~7U;
        uint8_t* output;
        posix_dirent64_t* entry;
        char child_path[512];
        fs_stat_t child_status;

        if (record_length > UINT16_MAX) return -POSIX_EIO;
        if (record_length > a3 - written) {
            if (!written) return -POSIX_EINVAL;
            break;
        }
        output = (uint8_t*)(uintptr_t)(a2 + written);
        memset(output, 0, record_length);
        entry = (posix_dirent64_t*)output;
        entry->inode = 0;
        entry->offset = (int64_t)descriptor->directory_offset + 1;
        entry->record_length = (uint16_t)record_length;
        entry->type = 0;
        if (strcmp(descriptor->path, "/") == 0) {
            strcpy(child_path, "/");
            memcpy(child_path + 1, names[i], name_length + 1);
        } else {
            size_t path_length = strlen(descriptor->path);
            if (path_length + 1 + name_length >= sizeof(child_path))
                return -POSIX_EIO;
            strcpy(child_path, descriptor->path);
            child_path[path_length] = '/';
            strcpy(child_path + path_length + 1, names[i]);
        }
        if (fs_stat(child_path, &child_status) &&
            child_status.type == FS_TYPE_DIR)
            entry->type = 4;
        else
            entry->type = 8;
        memcpy(entry->name, names[i], name_length + 1);
        written += (uint32_t)record_length;
        descriptor->directory_offset++;
    }
    return written;
}
REGISTER_SYSCALL(SYS_GETDENTS64, sys_getdents64, 3)

static int64_t sys_mkdir(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    extern int fs_mkdir(const char* path, uint16_t mode);
    
    char kpath[256];
    char resolved[256];
    if (!security_copy_user_string(kpath, sizeof(kpath),
                                   (const char*)a1, sizeof(kpath)) ||
        !syscall_resolve_user_path(kpath, resolved, sizeof(resolved), 0))
        return -1;
    if (!syscall_check_parent_write_access(resolved)) {
        kernel_user_t* user = kernel_user_current();
        fs_stat_t status;
        if (!user || strcmp(resolved, user->home) != 0 ||
            fs_stat(resolved, &status))
            return -1;
    }
    uint16_t create_mode = (uint16_t)a2 & 0777;
    create_mode &= (uint16_t)~syscall_file_creation_umask[
        syscall_current_console()];
    if (!fs_mkdir(resolved, create_mode))
        return -1;
    if (!syscall_assign_new_path_owner(resolved)) {
        fs_unlink(resolved);
        return -1;
    }
    return 1;
}
REGISTER_SYSCALL(SYS_MKDIR, sys_mkdir, 2)

static int64_t sys_unlink(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    extern int fs_unlink(const char* path);
    fs_stat_t status;
    char kpath[256];
    char resolved[256];
    if (!security_copy_user_string(kpath, sizeof(kpath),
                                   (const char*)a1, sizeof(kpath)))
        return -POSIX_EFAULT;
    if (!syscall_resolve_user_path(kpath, resolved, sizeof(resolved), 0))
        return -POSIX_EACCES;
    if (!fs_stat(resolved, &status))
        return -POSIX_ENOENT;
    if (status.type == FS_TYPE_DIR)
        return -POSIX_EISDIR;
    if (status.type != FS_TYPE_FILE)
        return -POSIX_EINVAL;
    if (!syscall_check_parent_write_access(resolved))
        return -POSIX_EACCES;
    return fs_unlink(resolved) ? 0 : -POSIX_EIO;
}
REGISTER_SYSCALL(SYS_UNLINK, sys_unlink, 1)

static int64_t sys_rename(uint64_t a1, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6)
{
    char old_path[256];
    char new_path[256];
    char resolved_old[256];
    char resolved_new[256];

    (void)a3; (void)a4; (void)a5; (void)a6;
    if (!security_copy_user_string(old_path, sizeof(old_path),
                                   (const char*)a1, sizeof(old_path)) ||
        !security_copy_user_string(new_path, sizeof(new_path),
                                   (const char*)a2, sizeof(new_path)) ||
        !syscall_resolve_user_path(old_path, resolved_old,
                                   sizeof(resolved_old), 0) ||
        !syscall_resolve_user_path(new_path, resolved_new,
                                   sizeof(resolved_new), 0) ||
        !syscall_check_parent_write_access(resolved_old) ||
        !syscall_check_parent_write_access(resolved_new))
        return -1;
    return fs_rename(resolved_old, resolved_new);
}
REGISTER_SYSCALL(SYS_RENAME, sys_rename, 2)

static int64_t sys_list(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    extern int fs_list(const char* path, char names[][CFS5_MAX_NAME], int max);
    
    char kpath[256];
    char resolved[256];
    if (a3 > 128 ||
        !security_copy_user_string(kpath, sizeof(kpath),
                                   (const char*)a1, sizeof(kpath)) ||
        !security_valid_user_write_range((const void*)a2,
                                         (size_t)a3 * CFS5_MAX_NAME) ||
        !syscall_resolve_user_path(kpath, resolved, sizeof(resolved), 1))
        return -1;

    if (!syscall_check_path_access(resolved, FS_PERM_READ | FS_PERM_EXEC))
        return -1;
    return fs_list(resolved, (char(*)[CFS5_MAX_NAME])a2, (int)a3);
}
REGISTER_SYSCALL(SYS_LIST, sys_list, 3)

static int64_t sys_chdir(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    extern int fs_chdir(const char* path);
    
    char resolved[256];
    char user_path[256];
    if (!security_copy_user_string(user_path, sizeof(user_path),
                                   (const char*)a1, sizeof(user_path)) ||
        !syscall_resolve_user_path(user_path, resolved, sizeof(resolved), 0))
        return -1;

    if (!syscall_check_path_access(resolved, FS_PERM_EXEC)) return -1;
    if (!fs_chdir(resolved)) return -1;
    tty_set_cwd(resolved);
    return 0;
}
REGISTER_SYSCALL(SYS_CHDIR, sys_chdir, 1)

static int64_t sys_chmod(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    extern int fs_chmod(const char* path, uint16_t mode);
    char input_path[256];
    char resolved[256];
    fs_stat_t status;
    kernel_user_t* user = kernel_user_current();

    if (a2 > 0777 ||
        !security_copy_user_string(input_path, sizeof(input_path),
                                   (const char*)a1, sizeof(input_path)) ||
        !syscall_resolve_user_path(input_path, resolved, sizeof(resolved), 0) ||
        !fs_stat(resolved, &status))
        return -1;

    if (!syscall_current_admin_allowed() &&
        (!user || kernel_user_uid(user) < 0 ||
         status.uid != (uint32_t)kernel_user_uid(user)))
        return -1;

    return fs_chmod(resolved, (uint16_t)a2) ? 0 : -1;
}
REGISTER_SYSCALL(SYS_CHMOD, sys_chmod, 2)

static int64_t sys_umask(uint64_t a1, uint64_t a2, uint64_t a3,
                         uint64_t a4, uint64_t a5, uint64_t a6)
{
    int console = syscall_current_console();
    uint16_t previous = syscall_file_creation_umask[console];
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;

    syscall_file_creation_umask[console] = (uint16_t)a1 & 0777;
    return previous;
}
REGISTER_SYSCALL(SYS_UMASK, sys_umask, 1)

static int64_t sys_access(uint64_t a1, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6)
{
    char input_path[256];
    char resolved[256];
    fs_stat_t status;
    (void)a3; (void)a4; (void)a5; (void)a6;

    if (a2 > (FS_PERM_READ | FS_PERM_WRITE | FS_PERM_EXEC))
        return -POSIX_EINVAL;
    if (!security_copy_user_string(input_path, sizeof(input_path),
                                   (const char*)a1, sizeof(input_path)))
        return -POSIX_EFAULT;
    if (!syscall_resolve_user_path(input_path, resolved, sizeof(resolved), 1))
        return -POSIX_EACCES;
    if (!fs_stat(resolved, &status))
        return -POSIX_ENOENT;
    if (!a2 || syscall_check_path_access(resolved, (int)a2))
        return 0;
    return -POSIX_EACCES;
}
REGISTER_SYSCALL(SYS_ACCESS, sys_access, 2)

static int64_t sys_chown(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    extern int fs_chown(const char* path, uint16_t uid, uint16_t gid);
    
    char kpath[256];
    if (!security_copy_from_user(kpath, (const void*)a1, sizeof(kpath))) return -1;
    kpath[sizeof(kpath) - 1] = '\0';

    if (!syscall_current_admin_allowed()) return -1;
    return fs_chown(kpath, (uint16_t)a2, (uint16_t)a3);
}
REGISTER_SYSCALL(SYS_CHOWN, sys_chown, 3)

static int64_t syscall_finish_user_child(int status)
{
    int depth = appl_user_child_depth();
    syscall_run_context_t* parent;
    int fork_slot;
    int parent_pid;
    int is_wait;
    uint64_t wait_status_pointer;
    int64_t wait_return_value;
    if (depth <= 0 || depth > SYSCALL_RUN_MAX_DEPTH)
        syscall_exit_halt("invalid synchronous child depth");

    parent = &syscall_run_contexts[depth - 1];
    fork_slot = parent->fork_slot;
    parent_pid = parent->parent_pid;
    is_wait = parent->is_wait;
    wait_status_pointer = parent->wait_status_pointer;
    tty_fullscreen_end();
    tty_newline_if_needed();
    if (!is_wait)
        syscall_process_transition();
    if (!appl_restore_user_parent())
        syscall_exit_halt("unable to restore synchronous child parent");
    if (!fs_chdir(parent->cwd) || !tty_set_cwd(parent->cwd))
        serial_puts("[syscall] warning: unable to restore parent working directory\n");
    syscall_file_creation_umask[syscall_current_console()] =
        parent->file_creation_umask;
    syscall_current_pid = parent_pid;
    wait_return_value = parent->child_pid;
    if (is_wait && wait_status_pointer) {
        int wait_status = ((int)(uint8_t)status) << 8;
        if (!security_copy_to_user((void*)(uintptr_t)wait_status_pointer,
                                   &wait_status, sizeof(wait_status)))
            wait_return_value = -POSIX_EFAULT;
    }

    pending_process_frame = (uint64_t)(uintptr_t)parent->syscall_frame;
    pending_process_user_rsp = parent->user_rsp;
    pending_process_return_value = (uint64_t)(int64_t)(
        is_wait ? wait_return_value : status);
    if (fork_slot >= 0 && fork_slot < SYSCALL_FORK_CHILD_MAX)
        memset(&syscall_fork_children[fork_slot], 0,
               sizeof(syscall_fork_children[fork_slot]));
    parent->user_rsp = 0;
    parent->cwd[0] = 0;
    parent->file_creation_umask = 0;
    parent->wait_status_pointer = 0;
    parent->fork_slot = -1;
    parent->is_wait = 0;
    return SYSCALL_PROCESS_RETURN;
}

static int64_t sys_getcwd(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    const char* src = tty_get_cwd();
    void* dst = (void*)a1;
    size_t len = strlen(src) + 1;

    if (a2 < len || !security_valid_user_range(dst, (size_t)a2)) return -1;
    if (!security_copy_to_user(dst, src, len)) return -1;
    return 0;
}
REGISTER_SYSCALL(SYS_CWD, sys_getcwd, 2)

static int64_t sys_fork(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    int slot;
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;

    if (current_rsp_saved <= VMM_USER_ADDRESS_BASE ||
        !security_valid_user_range(
            (const void*)(uintptr_t)(current_rsp_saved - 1), 1) ||
        !syscall_frame_pointer)
        return -POSIX_EINVAL;
    for (slot = 0; slot < SYSCALL_FORK_CHILD_MAX; slot++)
        if (!syscall_fork_children[slot].used) break;
    if (slot == SYSCALL_FORK_CHILD_MAX)
        return -POSIX_EAGAIN;
    if (syscall_next_pid <= 1 || syscall_next_pid == INT32_MAX)
        return -POSIX_EAGAIN;

    syscall_fork_child_t* child = &syscall_fork_children[slot];
    const char* cwd = tty_get_cwd();
    if (!cwd || strlen(cwd) >= sizeof(child->cwd))
        return -POSIX_EINVAL;
    memset(child, 0, sizeof(*child));
    if (!appl_fork_user_state(&child->user_state))
        return -POSIX_ENOMEM;
    memcpy(child->syscall_frame,
           (const void*)(uintptr_t)syscall_frame_pointer,
           sizeof(child->syscall_frame));
    child->user_rsp = current_rsp_saved;
    child->pid = syscall_next_pid++;
    child->parent_pid = syscall_current_pid;
    child->file_creation_umask =
        syscall_file_creation_umask[syscall_current_console()];
    strcpy(child->cwd, cwd);
    child->used = 1;
    return child->pid;
}
REGISTER_SYSCALL(SYS_FORK, sys_fork, 0)

static int64_t sys_run(uint64_t a1, uint64_t a2, uint64_t a3,
                       uint64_t a4, uint64_t a5, uint64_t a6)
{
    char input_path[256];
    char resolved[256];
    const char* cwd;
    int depth = appl_user_child_depth();
    syscall_run_context_t* context;
    int result;
    (void)a3; (void)a4; (void)a5; (void)a6;

    if (depth < 0 || depth >= SYSCALL_RUN_MAX_DEPTH)
        return -POSIX_EINVAL;
    if (!security_copy_user_string(input_path, sizeof(input_path),
                                   (const char*)a1, sizeof(input_path))) {
        serial_puts("[sys_run] invalid user path\n");
        return -POSIX_EFAULT;
    }
    if (!syscall_resolve_user_path(input_path, resolved, sizeof(resolved), 1)) {
        serial_puts("[sys_run] unable to resolve executable path\n");
        return -POSIX_EFAULT;
    }
    if (!syscall_check_path_access(resolved, FS_PERM_EXEC))
        return -POSIX_EACCES;
    if (!security_valid_user_argv((const char* const*)a2, 32, 256)) {
        serial_puts("[sys_run] invalid user argument vector\n");
        return -POSIX_EFAULT;
    }

    context = &syscall_run_contexts[depth];
    memset(context, 0, sizeof(*context));
    context->fork_slot = -1;
    context->parent_pid = syscall_current_pid;
    cwd = tty_get_cwd();
    if (!cwd || strlen(cwd) >= sizeof(context->cwd))
        return -POSIX_EINVAL;
    memcpy(context->syscall_frame,
           (const void*)(uintptr_t)syscall_frame_pointer,
           sizeof(context->syscall_frame));
    context->user_rsp = current_rsp_saved;
    strcpy(context->cwd, cwd);
    context->file_creation_umask =
        syscall_file_creation_umask[syscall_current_console()];

    result = appl_run_user_child(resolved, (char* const*)a2);
    if (result != (int)0xDEADBEEF) {
        syscall_file_creation_umask[syscall_current_console()] =
            context->file_creation_umask;
        memset(context, 0, sizeof(*context));
        return result;
    }
    syscall_process_transition();
    return result;
}
REGISTER_SYSCALL(SYS_RUN, sys_run, 2)

static int64_t sys_exec(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    char kpath[256];
    char resolved[256];
    if (!security_copy_user_string(kpath, sizeof(kpath),
                                   (const char*)a1, sizeof(kpath))) {
        serial_puts("[sys_exec] Invalid executable path pointer\n");
        return -1;
    }
    if (!syscall_resolve_user_path(kpath, resolved, sizeof(resolved), 1)) {
        serial_puts("[sys_exec] Executable path is outside the session boundary\n");
        return -1;
    }
    if (!security_valid_user_argv((const char* const*)a2, 32, 256)) {
        serial_puts("[sys_exec] Invalid argument vector\n");
        return -1;
    }

    if (!syscall_check_path_access(resolved, FS_PERM_EXEC)) {
        serial_puts("[sys_exec] Executable is not accessible: ");
        serial_puts(resolved);
        serial_puts("\n");
        return -1;
    }
    if (!kernel_user_current() && !strcmp(resolved, "/bin/sh")) {
        int rc = user_exec("/bin/login", (char* const*)a2);
        if ((uint32_t)rc != 0xDEADBEEF) {
            serial_puts("[sys_exec] bootstrap login fallback failed\n");
            return rc;
        }
        return (int64_t)0xDEADBEEF;
    }
    return user_exec_from_user(resolved, (char* const*)a2);
}
REGISTER_SYSCALL(SYS_EXEC, sys_exec, 2)

static int64_t sys_wait(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    int slot;
    int depth = appl_user_child_depth();
    syscall_run_context_t* context;
    const char* cwd;
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;

    if (a1 && !security_valid_user_write_range((void*)a1, sizeof(int)))
        return -POSIX_EFAULT;
    for (slot = 0; slot < SYSCALL_FORK_CHILD_MAX; slot++)
        if (syscall_fork_children[slot].used &&
            syscall_fork_children[slot].parent_pid == syscall_current_pid)
            break;
    if (slot == SYSCALL_FORK_CHILD_MAX)
        return -POSIX_ECHILD;
    if (depth < 0 || depth >= SYSCALL_RUN_MAX_DEPTH)
        return -POSIX_EAGAIN;
    context = &syscall_run_contexts[depth];
    cwd = tty_get_cwd();
    if (!cwd || strlen(cwd) >= sizeof(context->cwd))
        return -POSIX_EINVAL;
    memset(context, 0, sizeof(*context));
    memcpy(context->syscall_frame,
           (const void*)(uintptr_t)syscall_frame_pointer,
           sizeof(context->syscall_frame));
    context->user_rsp = current_rsp_saved;
    strcpy(context->cwd, cwd);
    context->file_creation_umask =
        syscall_file_creation_umask[syscall_current_console()];
    context->wait_status_pointer = a1;
    context->parent_pid = syscall_current_pid;
    context->child_pid = syscall_fork_children[slot].pid;
    context->fork_slot = slot;
    context->is_wait = 1;

    if (!appl_run_forked_user_child(
            &syscall_fork_children[slot].user_state)) {
        memset(context, 0, sizeof(*context));
        return -POSIX_ENOMEM;
    }
    syscall_file_creation_umask[syscall_current_console()] =
        syscall_fork_children[slot].file_creation_umask;
    if (!fs_chdir(syscall_fork_children[slot].cwd) ||
        !tty_set_cwd(syscall_fork_children[slot].cwd)) {
        (void)appl_restore_user_parent();
        (void)fs_chdir(context->cwd);
        (void)tty_set_cwd(context->cwd);
        syscall_file_creation_umask[syscall_current_console()] =
            context->file_creation_umask;
        memset(&syscall_fork_children[slot], 0,
               sizeof(syscall_fork_children[slot]));
        memset(context, 0, sizeof(*context));
        return -POSIX_EIO;
    }
    syscall_current_pid = context->child_pid;
    pending_process_frame =
        (uint64_t)(uintptr_t)syscall_fork_children[slot].syscall_frame;
    pending_process_user_rsp = syscall_fork_children[slot].user_rsp;
    pending_process_return_value = 0;
    return SYSCALL_PROCESS_RETURN;
}
REGISTER_SYSCALL(SYS_WAIT, sys_wait, 1)

static int64_t sys_getpid(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    return syscall_current_pid;
}
REGISTER_SYSCALL(SYS_GETPID, sys_getpid, 0)

static int64_t sys_yield(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    return -1;
}
REGISTER_SYSCALL(SYS_YIELD, sys_yield, 0)

static int64_t sys_kill(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    return -1;
}
REGISTER_SYSCALL(SYS_KILL, sys_kill, 1)

static int64_t sys_user_add(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    char name[32], pass[64] = { 0 };
    fs_stat_t firstboot_marker;
    if (!security_copy_user_string(name, sizeof(name), (const char*)a1,
                                   sizeof(name)) ||
        !security_copy_user_string(pass, sizeof(pass), (const char*)a2,
                                   sizeof(pass))) {
        memset(pass, 0, sizeof(pass));
        return -1;
    }

    kernel_user_t* current = kernel_user_current();
    if (a3 > 3) {
        memset(pass, 0, sizeof(pass));
        return -1;
    }
    if (current) {
        if (!syscall_current_admin_allowed() || a3 == 3 ||
            !strcmp(current->name, name)) {
            memset(pass, 0, sizeof(pass));
            return USER_ADD_PERMISSION_DENIED;
        }
    } else if (kernel_user_count() != 0 || a3 != 3 ||
               fs_stat("/etc/.firstboot_done", &firstboot_marker)) {
        memset(pass, 0, sizeof(pass));
        return USER_ADD_PERMISSION_DENIED;
    }
    int bootstrap = current == NULL;
    int result = kernel_user_add(name, pass, (int)a3);
    memset(pass, 0, sizeof(pass));
    if (result > 0 && bootstrap) {
        kernel_user_t* created = kernel_user_current();
        if (!created || strcmp(created->name, name) != 0) {
            serial_puts("[users] ERROR: initial account was not activated\n");
            return -1;
        }
        syscall_process_transition();
        tty_set_session_active(1);
    }
    return result;
}
REGISTER_SYSCALL(SYS_USER_ADD, sys_user_add, 3)

static int64_t sys_user_login(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    char name[32], pass[64] = { 0 };
    if (kernel_user_current())
        return -POSIX_EACCES;
    if (!security_copy_user_string(name, sizeof(name), (const char*)a1,
                                   sizeof(name)) ||
        !security_copy_user_string(pass, sizeof(pass), (const char*)a2,
                                   sizeof(pass))) {
        memset(pass, 0, sizeof(pass));
        return -1;
    }

    int result = kernel_user_login(name, pass);
    memset(pass, 0, sizeof(pass));
    if (result > 0) {
        syscall_process_transition();
        syscall_clear_shell_history(syscall_current_console());
        tty_set_session_active(1);
    }
    return result;
}
REGISTER_SYSCALL(SYS_USER_LOGIN, sys_user_login, 2)

static int64_t sys_user_verify_password(uint64_t a1, uint64_t a2,
                                       uint64_t a3, uint64_t a4,
                                       uint64_t a5, uint64_t a6)
{
    char pass[64] = { 0 };
    int result;
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;

    if (!kernel_user_current() ||
        !security_copy_user_string(pass, sizeof(pass), (const char*)a1,
                                   sizeof(pass)))
        return -POSIX_EACCES;
    result = kernel_user_verify_current_password(pass);
    memset(pass, 0, sizeof(pass));
    return result;
}
REGISTER_SYSCALL(SYS_USER_VERIFY_PASSWORD, sys_user_verify_password, 1)

static int64_t sys_user_logout(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    if (!kernel_user_current()) return -1;
    syscall_process_transition();
    kernel_user_logout();
    syscall_clear_shell_history(syscall_current_console());
    tty_set_cwd("/");
    tty_set_session_active(0);
    return 0;
}
REGISTER_SYSCALL(SYS_USER_LOGOUT, sys_user_logout, 0)

static int64_t sys_user_find(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    kernel_user_t* user = NULL;
    kernel_user_t result;
    char name[32];

    if (!security_copy_user_string(name, sizeof(name), (const char*)a1,
                                   sizeof(name)) ||
        !security_valid_user_range((void*)a2, sizeof(result))) return -1;

    int found = kernel_user_find(name, &user);
    if (found) {
        result = *user;
        memset(result.password_hash, 0, sizeof(result.password_hash));
    } else {
        memset(&result, 0, sizeof(result));
    }
    if (!security_copy_to_user((void*)a2, &result, sizeof(result))) return -1;
    return found;
}
REGISTER_SYSCALL(SYS_USER_FIND, sys_user_find, 2)

static int64_t sys_user_setpass(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    char name[32], pass[64] = { 0 }, current_pass[64] = { 0 };
    kernel_user_t* current;
    kernel_user_t* target = NULL;
    if (!security_copy_user_string(name, sizeof(name), (const char*)a1,
                                   sizeof(name)) ||
        !security_copy_user_string(pass, sizeof(pass), (const char*)a2,
                                   sizeof(pass))) {
        memset(pass, 0, sizeof(pass));
        memset(current_pass, 0, sizeof(current_pass));
        return -1;
    }

    current = kernel_user_current();
    if (!current || !kernel_user_find(name, &target)) {
        memset(pass, 0, sizeof(pass));
        memset(current_pass, 0, sizeof(current_pass));
        return -1;
    }
    if (!syscall_current_admin_allowed()) {
        if (current != target ||
            !security_copy_user_string(current_pass, sizeof(current_pass),
                                       (const char*)a3,
                                       sizeof(current_pass)) ||
            !kernel_user_verify_current_password(current_pass)) {
            memset(pass, 0, sizeof(pass));
            memset(current_pass, 0, sizeof(current_pass));
            return -POSIX_EACCES;
        }
    }
    int result = kernel_user_setpass(name, pass);
    memset(pass, 0, sizeof(pass));
    memset(current_pass, 0, sizeof(current_pass));
    return result;
}
REGISTER_SYSCALL(SYS_USER_SETPASS, sys_user_setpass, 3)

static int64_t sys_user_current(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    if (!security_valid_user_range((void*)a1, sizeof(kernel_user_t))) return -1;
    kernel_user_t* curr = kernel_user_current();
    if (!curr) return 0;

    kernel_user_t user_copy = *curr;
    memset(user_copy.password_hash, 0, sizeof(user_copy.password_hash));
    return security_copy_to_user((void*)a1, &user_copy, sizeof(user_copy)) ? 1 : -1;
}
REGISTER_SYSCALL(SYS_USER_CURRENT, sys_user_current, 1)

static int64_t sys_user_lock(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    char name[32];
    kernel_user_t* target = NULL;
    if (!security_copy_user_string(name, sizeof(name), (const char*)a1,
                                   sizeof(name))) return -1;

    if (!syscall_current_admin_allowed() ||
        !kernel_user_find(name, &target) || target->role == 3 ||
        target == kernel_user_current())
        return -1;
    return kernel_user_set_locked(name, 1);
}
REGISTER_SYSCALL(SYS_USER_LOCK, sys_user_lock, 1)

static int64_t sys_user_unlock(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    char name[32];
    if (!security_copy_user_string(name, sizeof(name), (const char*)a1,
                                   sizeof(name))) return -1;

    if (!syscall_current_admin_allowed()) return -1;
    return kernel_user_set_locked(name, 0);
}
REGISTER_SYSCALL(SYS_USER_UNLOCK, sys_user_unlock, 1)

static int64_t sys_user_disable(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    char name[32];
    kernel_user_t* target = NULL;
    if (!security_copy_user_string(name, sizeof(name), (const char*)a1,
                                   sizeof(name))) return -1;

    if (!syscall_current_admin_allowed() ||
        !kernel_user_find(name, &target) || target->role == 3 ||
        target == kernel_user_current())
        return -1;
    return kernel_user_set_disabled(name, 1);
}
REGISTER_SYSCALL(SYS_USER_DISABLE, sys_user_disable, 1)

static int64_t sys_user_enable(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    char name[32];
    if (!security_copy_user_string(name, sizeof(name), (const char*)a1,
                                   sizeof(name))) return -1;

    if (!syscall_current_admin_allowed()) return -1;
    return kernel_user_set_disabled(name, 0);
}
REGISTER_SYSCALL(SYS_USER_ENABLE, sys_user_enable, 1)

static int64_t sys_user_count(uint64_t a1, uint64_t a2, uint64_t a3,
                              uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    return kernel_user_count();
}
REGISTER_SYSCALL(SYS_USER_COUNT, sys_user_count, 0)

static int64_t sys_user_get(uint64_t a1, uint64_t a2, uint64_t a3,
                            uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    kernel_user_t user;
    if (!syscall_current_admin_allowed() ||
        a1 > (uint64_t)INT32_MAX ||
        !security_valid_user_range((void*)a2, sizeof(user)) ||
        !kernel_user_get((int)a1, &user))
        return -1;
    return security_copy_to_user((void*)a2, &user, sizeof(user)) ? 1 : -1;
}
REGISTER_SYSCALL(SYS_USER_GET, sys_user_get, 2)

static int64_t sys_user_setrole(uint64_t a1, uint64_t a2, uint64_t a3,
                                uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    char name[32];
    kernel_user_t* current = kernel_user_current();
    kernel_user_t* target = NULL;
    if (!current || !syscall_current_admin_allowed() ||
        !security_copy_user_string(name, sizeof(name), (const char*)a1,
                                   sizeof(name)) ||
        a2 > 2)
        return -1;
    if (!kernel_user_find(name, &target) || target->role == 3 ||
        target == current)
        return -1;
    return kernel_user_set_role(name, (int)a2);
}
REGISTER_SYSCALL(SYS_USER_SETROLE, sys_user_setrole, 2)

static int64_t sys_ticks(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    extern volatile uint64_t g_ticks;
    return (int64_t)g_ticks;
}
REGISTER_SYSCALL(SYS_TICKS, sys_ticks, 0)

static int64_t sys_sleep(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    extern volatile uint64_t g_ticks;
    uint64_t milliseconds = a1;
    uint64_t ticks = (milliseconds + 9) / 10;
    
    if (milliseconds > UINT64_MAX - 9 || ticks > UINT64_MAX - g_ticks)
        return -1;
        
    uint64_t target = g_ticks + ticks;
    while (g_ticks < target) {
        __asm__ volatile("sti; hlt; cli");
    }
    return 0;
}
REGISTER_SYSCALL(SYS_SLEEP, sys_sleep, 1)

static int64_t sys_time(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    rtc_time_t ktime;
    if (!security_valid_user_range((void*)a1, sizeof(rtc_time_t))) return -1;
    
    if (rtc_read_time(&ktime)) {
        if (security_copy_to_user((void*)a1, &ktime, sizeof(rtc_time_t))) return 0;
    }
    return -1;
}
REGISTER_SYSCALL(SYS_TIME, sys_time, 1)

static int64_t sys_power_info(uint64_t a1, uint64_t a2, uint64_t a3,
                              uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    return acpi_s3_available() ? 1 : 0;
}
REGISTER_SYSCALL(SYS_POWER_INFO, sys_power_info, 0)

static int64_t sys_power_set_idle(uint64_t a1, uint64_t a2, uint64_t a3,
                                  uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    if (!syscall_current_admin_allowed() || a1 > UINT32_MAX)
        return -1;
    return acpi_idle_set_timeout((uint32_t)a1) ? 0 : -1;
}
REGISTER_SYSCALL(SYS_POWER_SET_IDLE, sys_power_set_idle, 1)

static int64_t sys_pci_info(uint64_t a1, uint64_t a2, uint64_t a3,
                            uint64_t a4, uint64_t a5, uint64_t a6)
{
    const pci_device_t* device;
    pci_info_t info;
    (void)a4; (void)a5; (void)a6;

    if (a1 == 0)
        return pci_device_count();
    if (a1 != 1 || a2 >= (uint64_t)pci_device_count() || !a3 ||
        !security_valid_user_range((void*)a3, sizeof(info)))
        return -1;
    device = pci_device_at((int)a2);
    if (!device)
        return -1;
    memset(&info, 0, sizeof(info));
    info.bus = device->bus;
    info.slot = device->slot;
    info.function = device->function;
    info.class_code = device->class_code;
    info.subclass = device->subclass;
    info.prog_if = device->prog_if;
    info.vendor = device->vendor;
    info.device = device->device;
    return security_copy_to_user((void*)a3, &info, sizeof(info)) ? 0 : -1;
}
REGISTER_SYSCALL(SYS_PCI_INFO, sys_pci_info, 3)

static int64_t sys_monotonic_ns(uint64_t a1, uint64_t a2, uint64_t a3,
                                uint64_t a4, uint64_t a5, uint64_t a6)
{
    extern volatile uint64_t g_ticks;
    uint64_t nanoseconds;
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;

    if (hpet_monotonic_ns(&nanoseconds))
        return nanoseconds <= INT64_MAX ? (int64_t)nanoseconds : -1;
    if (g_ticks > (uint64_t)INT64_MAX / 10000000ULL)
        return -1;
    return (int64_t)(g_ticks * 10000000ULL);
}
REGISTER_SYSCALL(SYS_MONOTONIC_NS, sys_monotonic_ns, 0)

static int64_t sys_mem_alloc(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    return (int64_t)(uintptr_t)appl_user_alloc(a1);
}
REGISTER_SYSCALL(SYS_ALLOC, sys_mem_alloc, 1)

static int64_t sys_mem_free(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    return appl_user_free((void*)(uintptr_t)a1) ? 0 : -1;
}
REGISTER_SYSCALL(SYS_FREE, sys_mem_free, 1)

static int64_t sys_debug(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    vga_print("[syscall] debug: syscall interface working!\n", 0x0E);
    return 0;
}
REGISTER_SYSCALL(SYS_DEBUG, sys_debug, 0)

static int64_t sys_statfs(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    extern int fs_get_statfs(const char* path, statfs_t* buf);
    
    char kpath[256];
    char resolved_path[256];
    statfs_t kbuf;

    if (!security_copy_user_string(kpath, sizeof(kpath),
                                   (const char*)a1, sizeof(kpath)) ||
        !security_valid_user_range((void*)a2, sizeof(statfs_t)) ||
        !syscall_resolve_user_path(kpath, resolved_path,
                                   sizeof(resolved_path), 1) ||
        !syscall_check_path_access(resolved_path, FS_PERM_READ))
        return -1;

    int res = fs_get_statfs(resolved_path, &kbuf);
    if (res == 0) {
        if (!security_copy_to_user((void*)a2, &kbuf, sizeof(statfs_t))) return -1;
    }
    return res;
}
REGISTER_SYSCALL(SYS_STATFS, sys_statfs, 2)

static int64_t sys_fsck(uint64_t a1, uint64_t a2, uint64_t a3,
                        uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    if (!syscall_current_admin_allowed())
        return -1;
    return cfs5_fsck_run() ? 0 : -1;
}
REGISTER_SYSCALL(SYS_FSCK, sys_fsck, 0)

static int64_t sys_usb_info(uint64_t a1, uint64_t a2, uint64_t a3,
                            uint64_t a4, uint64_t a5, uint64_t a6)
{
    usb_info_t info;
    const char* name;
    (void)a5; (void)a6;

    if (!a4 || !security_valid_user_range((void*)a4, sizeof(info)))
        return -1;
    memset(&info, 0, sizeof(info));
    info.controller_count = usb_controller_count();
    if (a1 == 0) {
        if (!security_copy_to_user((void*)a4, &info, sizeof(info))) return -1;
        return 0;
    }
    if (a1 == 1) {
        if (a2 >= (uint64_t)info.controller_count) return -1;
        info.controller_ready = usb_controller_ready_at((int)a2);
        info.transfer_ready = usb_storage_ready_at((int)a2);
        info.port_count = usb_port_count((int)a2);
        info.partition_count = usb_partition_count_at((int)a2);
        info.storage_sector_count =
            usb_storage_sector_count_at((int)a2);
        name = usb_controller_name_at((int)a2);
        for (size_t i = 0; i < sizeof(info.controller_name) - 1 && name[i]; i++)
            info.controller_name[i] = name[i];
        name = usb_storage_name_at((int)a2);
        for (size_t i = 0; i < sizeof(info.storage_name) - 1 && name[i]; i++)
            info.storage_name[i] = name[i];
    } else if (a1 == 2) {
        if (a2 >= (uint64_t)info.controller_count ||
            !usb_controller_ready_at((int)a2) ||
            a3 >= (uint64_t)usb_port_count((int)a2))
            return -1;
        info.controller_ready = 1;
        info.transfer_ready = usb_storage_ready_at((int)a2);
        info.port_count = usb_port_count((int)a2);
        info.connected = usb_port_connected((int)a2, (int)a3);
    } else if (a1 == 3) {
        if (a2 >= (uint64_t)info.controller_count ||
            a3 >= (uint64_t)usb_partition_count_at((int)a2) ||
            !usb_partition_info_at((int)a2, (int)a3,
                                   info.partition_name,
                                   &info.partition_first_lba,
                                   &info.partition_sector_count))
            return -1;
        info.controller_ready = usb_controller_ready_at((int)a2);
        info.transfer_ready = 1;
        info.port_count = usb_port_count((int)a2);
        info.partition_count = usb_partition_count_at((int)a2);
    } else {
        return -1;
    }
    return security_copy_to_user((void*)a4, &info, sizeof(info)) ? 0 : -1;
}
REGISTER_SYSCALL(SYS_USB_INFO, sys_usb_info, 4)

static int64_t sys_net_count(uint64_t a1, uint64_t a2, uint64_t a3,
                             uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    return netdev_count();
}
REGISTER_SYSCALL(SYS_NET_COUNT, sys_net_count, 0)

static int64_t sys_net_info(uint64_t a1, uint64_t a2, uint64_t a3,
                            uint64_t a4, uint64_t a5, uint64_t a6)
{
    netdev_info_t info;
    (void)a3; (void)a4; (void)a5; (void)a6;

    if (a1 >= (uint64_t)netdev_count() ||
        !security_valid_user_range((void*)a2, sizeof(info)))
        return -1;
    net_stack_poll();
    if (!netdev_get_info((int)a1, &info)) return -1;
    return security_copy_to_user((void*)a2, &info, sizeof(info)) ? 0 : -1;
}
REGISTER_SYSCALL(SYS_NET_INFO, sys_net_info, 2)

static int64_t sys_net_dhcp(uint64_t a1, uint64_t a2, uint64_t a3,
                            uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    if (!syscall_network_admin_or_bootstrap_allowed() ||
        a1 >= (uint64_t)netdev_count())
        return -1;
    return net_dhcp_start((int)a1);
}
REGISTER_SYSCALL(SYS_NET_DHCP, sys_net_dhcp, 1)

static int64_t sys_net_ping(uint64_t a1, uint64_t a2, uint64_t a3,
                            uint64_t a4, uint64_t a5, uint64_t a6)
{
    uint8_t address[4];
    uint32_t round_trip_ms;
    int result;
    (void)a4; (void)a5; (void)a6;

    if (!syscall_network_admin_allowed() ||
        a1 >= (uint64_t)netdev_count() ||
        a3 == 0 || a3 > 10000 ||
        !security_valid_user_range((void*)a2, sizeof(address)) ||
        !security_copy_from_user(address, (const void*)a2, sizeof(address)))
        return -1;
    __asm__ volatile("sti" ::: "memory");
    result = net_ping_ipv4((int)a1, address, (uint32_t)a3,
                           &round_trip_ms);
    __asm__ volatile("cli" ::: "memory");
    if (result == -2)
        return sys_exit(130, 0, 0, 0, 0, 0);
    if (result < 0) return -1;
    return result ? (int64_t)round_trip_ms + 1 : 0;
}
REGISTER_SYSCALL(SYS_NET_PING, sys_net_ping, 3)

static int64_t sys_net_tcp(uint64_t a1, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6)
{
    uint8_t address[4];
    uint32_t response_length = 0;
    int result;

    if (!syscall_network_admin_allowed() || net_tcp_syscall_busy ||
        a1 >= (uint64_t)netdev_count() ||
        !a3 || a3 > UINT16_MAX || a5 > NET_TCP_MAX_REQUEST ||
        !security_valid_user_range((void*)a2, sizeof(address)) ||
        !security_copy_from_user(address, (const void*)a2, sizeof(address)) ||
        (a5 && (!security_valid_user_range((void*)a4, (size_t)a5) ||
                !security_copy_from_user(net_tcp_request_buffer,
                                         (const void*)a4, (size_t)a5))) ||
        !security_valid_user_write_range((void*)a6,
                                         NET_TCP_MAX_RESPONSE))
        return -1;

    net_tcp_syscall_busy = 1;
    __asm__ volatile("sti" ::: "memory");
    result = net_tcp_exchange_ipv4((int)a1, address, (uint16_t)a3,
                                   net_tcp_request_buffer, (uint32_t)a5,
                                   net_tcp_response_buffer,
                                   sizeof(net_tcp_response_buffer),
                                   15000, &response_length);
    __asm__ volatile("cli" ::: "memory");
    if (result == 0 &&
        !security_copy_to_user((void*)a6, net_tcp_response_buffer,
                               response_length))
        result = -1;
    net_tcp_syscall_busy = 0;
    if (result == -2)
        return sys_exit(130, 0, 0, 0, 0, 0);
    return result < 0 ? -1 : (int64_t)response_length;
}
REGISTER_SYSCALL(SYS_NET_TCP, sys_net_tcp, 6)

static int64_t sys_net_tcp_stream_open(uint64_t a1, uint64_t a2,
                                       uint64_t a3, uint64_t a4,
                                       uint64_t a5, uint64_t a6)
{
    uint8_t address[4];
    int result;
    (void)a5; (void)a6;
    if (!syscall_network_admin_allowed() || net_tcp_syscall_busy ||
        a1 >= (uint64_t)netdev_count() ||
        a3 == 0 || a3 > UINT16_MAX || a4 == 0 || a4 > 30000 ||
        !security_valid_user_range((const void*)a2, sizeof(address)) ||
        !security_copy_from_user(address, (const void*)a2, sizeof(address)))
        return -1;
    net_tcp_syscall_busy = 1;
    __asm__ volatile("sti" ::: "memory");
    result = net_tcp_stream_open_ipv4((int)a1, address, (uint16_t)a3,
                                      (uint32_t)a4);
    __asm__ volatile("cli" ::: "memory");
    if (result >= 0)
    {
        net_tcp_stream_owner_console = syscall_current_console();
        net_tcp_stream_owner_generation =
            security_process_generation(net_tcp_stream_owner_console);
        net_tcp_stream_id = (int)result;
    }
    net_tcp_syscall_busy = 0;
    if (result == -2)
        return sys_exit(130, 0, 0, 0, 0, 0);
    return result;
}
REGISTER_SYSCALL(SYS_NET_TCP_STREAM_OPEN, sys_net_tcp_stream_open, 4)

static int64_t sys_net_tcp_stream_write(uint64_t a1, uint64_t a2,
                                        uint64_t a3, uint64_t a4,
                                        uint64_t a5, uint64_t a6)
{
    int result;
    (void)a5; (void)a6;
    if (!syscall_network_admin_allowed() || net_tcp_syscall_busy ||
        a1 == 0 ||
        a1 > INT32_MAX || (int)a1 != net_tcp_stream_id ||
        net_tcp_stream_owner_console != syscall_current_console() ||
        !security_process_generation_matches(
            syscall_current_console(), net_tcp_stream_owner_generation) ||
        a3 == 0 ||
        a3 > NET_TCP_STREAM_IO_MAX || a4 == 0 || a4 > 30000 ||
        !security_valid_user_range((const void*)a2, (size_t)a3) ||
        !security_copy_from_user(net_tcp_stream_buffer, (const void*)a2,
                                 (size_t)a3))
        return -1;
    net_tcp_syscall_busy = 1;
    __asm__ volatile("sti" ::: "memory");
    result = net_tcp_stream_write((int)a1, net_tcp_stream_buffer,
                                  (uint32_t)a3, (uint32_t)a4);
    __asm__ volatile("cli" ::: "memory");
    memset(net_tcp_stream_buffer, 0, (size_t)a3);
    net_tcp_syscall_busy = 0;
    if (result == -2)
        return sys_exit(130, 0, 0, 0, 0, 0);
    return result;
}
REGISTER_SYSCALL(SYS_NET_TCP_STREAM_WRITE, sys_net_tcp_stream_write, 4)

static int64_t sys_net_tcp_stream_read(uint64_t a1, uint64_t a2,
                                       uint64_t a3, uint64_t a4,
                                       uint64_t a5, uint64_t a6)
{
    int result;
    (void)a5; (void)a6;
    if (!syscall_network_admin_allowed() || net_tcp_syscall_busy ||
        a1 == 0 ||
        a1 > INT32_MAX || (int)a1 != net_tcp_stream_id ||
        net_tcp_stream_owner_console != syscall_current_console() ||
        !security_process_generation_matches(
            syscall_current_console(), net_tcp_stream_owner_generation) ||
        a3 == 0 ||
        a3 > NET_TCP_STREAM_IO_MAX || a4 == 0 || a4 > 30000 ||
        !security_valid_user_write_range((void*)a2, (size_t)a3))
        return -1;
    net_tcp_syscall_busy = 1;
    __asm__ volatile("sti" ::: "memory");
    result = net_tcp_stream_read((int)a1, net_tcp_stream_buffer,
                                 (uint32_t)a3, (uint32_t)a4);
    __asm__ volatile("cli" ::: "memory");
    if (result > 0 &&
        !security_copy_to_user((void*)a2, net_tcp_stream_buffer,
                               (size_t)result))
        result = -1;
    if (result > 0)
        memset(net_tcp_stream_buffer, 0, (size_t)result);
    net_tcp_syscall_busy = 0;
    if (result == -2)
        return sys_exit(130, 0, 0, 0, 0, 0);
    return result;
}
REGISTER_SYSCALL(SYS_NET_TCP_STREAM_READ, sys_net_tcp_stream_read, 4)

static int64_t sys_net_tcp_stream_close(uint64_t a1, uint64_t a2,
                                        uint64_t a3, uint64_t a4,
                                        uint64_t a5, uint64_t a6)
{
    int result;
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    if (net_tcp_syscall_busy || a1 == 0 || a1 > INT32_MAX ||
        (int)a1 != net_tcp_stream_id ||
        net_tcp_stream_owner_console != syscall_current_console() ||
        !security_process_generation_matches(
            syscall_current_console(), net_tcp_stream_owner_generation))
        return -1;
    net_tcp_syscall_busy = 1;
    __asm__ volatile("sti" ::: "memory");
    result = net_tcp_stream_close((int)a1);
    __asm__ volatile("cli" ::: "memory");
    if (result == 0) {
        net_tcp_stream_owner_console = -1;
        net_tcp_stream_owner_generation = 0;
        net_tcp_stream_id = 0;
    }
    net_tcp_syscall_busy = 0;
    return result;
}
REGISTER_SYSCALL(SYS_NET_TCP_STREAM_CLOSE, sys_net_tcp_stream_close, 1)

static int64_t sys_net_tcp_listen(uint64_t a1, uint64_t a2, uint64_t a3,
                                  uint64_t a4, uint64_t a5, uint64_t a6)
{
    int result;
    (void)a3; (void)a4; (void)a5; (void)a6;
    if (!syscall_network_admin_allowed() || net_tcp_syscall_busy ||
        net_tcp_listener_id || a1 >= (uint64_t)netdev_count() ||
        a2 == 0 || a2 > UINT16_MAX)
        return -1;
    net_tcp_syscall_busy = 1;
    __asm__ volatile("sti" ::: "memory");
    result = net_tcp_listener_open_ipv4((int)a1, (uint16_t)a2);
    __asm__ volatile("cli" ::: "memory");
    if (result > 0) {
        net_tcp_listener_owner_console = syscall_current_console();
        net_tcp_listener_owner_generation =
            security_process_generation(net_tcp_listener_owner_console);
        net_tcp_listener_id = result;
    }
    net_tcp_syscall_busy = 0;
    return result;
}
REGISTER_SYSCALL(SYS_NET_TCP_LISTEN, sys_net_tcp_listen, 2)

static int64_t sys_net_tcp_accept(uint64_t a1, uint64_t a2, uint64_t a3,
                                  uint64_t a4, uint64_t a5, uint64_t a6)
{
    int result;
    (void)a3; (void)a4; (void)a5; (void)a6;
    if (!syscall_network_admin_allowed() || net_tcp_syscall_busy ||
        a1 == 0 || a1 > INT32_MAX || (int)a1 != net_tcp_listener_id ||
        net_tcp_listener_owner_console != syscall_current_console() ||
        !security_process_generation_matches(
            syscall_current_console(), net_tcp_listener_owner_generation) ||
        a2 == 0 || a2 > 30000 || net_tcp_stream_id)
        return -1;
    net_tcp_syscall_busy = 1;
    __asm__ volatile("sti" ::: "memory");
    result = net_tcp_listener_accept((int)a1, (uint32_t)a2);
    __asm__ volatile("cli" ::: "memory");
    if (result > 0) {
        net_tcp_stream_owner_console = syscall_current_console();
        net_tcp_stream_owner_generation =
            security_process_generation(net_tcp_stream_owner_console);
        net_tcp_stream_id = result;
    }
    net_tcp_syscall_busy = 0;
    if (result == -2)
        return sys_exit(130, 0, 0, 0, 0, 0);
    return result;
}
REGISTER_SYSCALL(SYS_NET_TCP_ACCEPT, sys_net_tcp_accept, 2)

static int64_t sys_net_tcp_listener_close(uint64_t a1, uint64_t a2,
                                          uint64_t a3, uint64_t a4,
                                          uint64_t a5, uint64_t a6)
{
    int result;
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    if (net_tcp_syscall_busy || a1 == 0 || a1 > INT32_MAX ||
        (int)a1 != net_tcp_listener_id ||
        net_tcp_listener_owner_console != syscall_current_console() ||
        !security_process_generation_matches(
            syscall_current_console(),
            net_tcp_listener_owner_generation))
        return -1;
    net_tcp_syscall_busy = 1;
    __asm__ volatile("sti" ::: "memory");
    result = net_tcp_listener_close((int)a1);
    __asm__ volatile("cli" ::: "memory");
    if (result == 0) {
        if (net_tcp_stream_owner_console == syscall_current_console() &&
            net_tcp_stream_owner_generation ==
                security_process_generation(syscall_current_console())) {
            net_tcp_stream_owner_console = -1;
            net_tcp_stream_owner_generation = 0;
            net_tcp_stream_id = 0;
        }
        net_tcp_listener_owner_console = -1;
        net_tcp_listener_owner_generation = 0;
        net_tcp_listener_id = 0;
    }
    net_tcp_syscall_busy = 0;
    return result;
}
REGISTER_SYSCALL(SYS_NET_TCP_LISTENER_CLOSE,
                 sys_net_tcp_listener_close, 1)

static int64_t sys_random(uint64_t a1, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6)
{
    uint8_t bytes[256];
    (void)a3; (void)a4; (void)a5; (void)a6;
    if (a2 == 0 || a2 > sizeof(bytes) ||
        !security_valid_user_write_range((void*)a1, (size_t)a2))
        return -1;
    if (!x86_64_random_bytes(bytes, (size_t)a2)) {
        memset(bytes, 0, sizeof(bytes));
        return -1;
    }
    if (!security_copy_to_user((void*)a1, bytes, (size_t)a2)) {
        memset(bytes, 0, sizeof(bytes));
        return -1;
    }
    memset(bytes, 0, sizeof(bytes));
    return (int64_t)a2;
}
REGISTER_SYSCALL(SYS_RANDOM, sys_random, 2)

static int64_t sys_net_resolve(uint64_t a1, uint64_t a2, uint64_t a3,
                               uint64_t a4, uint64_t a5, uint64_t a6)
{
    char hostname[NET_DNS_MAX_NAME_LENGTH + 1];
    uint8_t address[4];
    int result;
    (void)a4; (void)a5; (void)a6;

    if (!syscall_network_admin_allowed() || net_dns_syscall_busy ||
        a1 >= (uint64_t)netdev_count() ||
        !security_copy_user_string(hostname, sizeof(hostname),
                                   (const char*)a2, sizeof(hostname)) ||
        !security_valid_user_write_range((void*)a3, sizeof(address)))
        return -1;

    net_dns_syscall_busy = 1;
    __asm__ volatile("sti" ::: "memory");
    result = net_resolve_ipv4((int)a1, hostname, 5000, address);
    __asm__ volatile("cli" ::: "memory");
    memset(hostname, 0, sizeof(hostname));
    net_dns_syscall_busy = 0;
    if (result == -2)
        return sys_exit(130, 0, 0, 0, 0, 0);
    if (result <= 0) return result;
    return security_copy_to_user((void*)a3, address, sizeof(address)) ? 1 : -1;
}
REGISTER_SYSCALL(SYS_NET_RESOLVE, sys_net_resolve, 3)

static int64_t sys_kernel_version(uint64_t a1, uint64_t a2, uint64_t a3,
                                  uint64_t a4, uint64_t a5, uint64_t a6)
{
    const createos_kernel_version_t version = {
        .abi_version = CREATEOS_VERSION_ABI,
        .name = CREATEOS_NAME,
        .release = CREATEOS_KERNEL_VERSION,
        .codename = CREATEOS_CODENAME,
        .architecture = CREATEOS_ARCH,
        .build_date = CREATEOS_BUILD_DATE
    };
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;

    if (!security_valid_user_range((void*)a1, sizeof(version)))
        return -1;
    return security_copy_to_user((void*)a1, &version, sizeof(version)) ? 0 : -1;
}
REGISTER_SYSCALL(SYS_KERNEL_VERSION, sys_kernel_version, 1)

static int64_t sys_tty_name(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    const char* name = tty_name();
    void* out = (void*)a1;
    
    if (!security_valid_user_range(out, 8)) return -1;
    if (!security_copy_to_user(out, name, 8)) return -1;
    return 0;
}
REGISTER_SYSCALL(SYS_TTY_NAME, sys_tty_name, 1)

static int64_t sys_tty_activate(uint64_t a1, uint64_t a2, uint64_t a3,
                                uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    char name[16];
    if (!security_copy_user_string(name, sizeof(name), (const char*)a1, sizeof(name)))
        return -1;
    return tty_activate_by_name(name);
}
REGISTER_SYSCALL(SYS_TTY_ACTIVATE, sys_tty_activate, 1)

static int64_t sys_tty_info(uint64_t a1, uint64_t a2, uint64_t a3,
                            uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    tty_info_t info;
    if (a1 >= (uint64_t)tty_console_count() ||
        !security_valid_user_range((void*)a2, sizeof(info)) ||
        tty_get_info((int)a1, &info) < 0)
        return -1;
    return security_copy_to_user((void*)a2, &info, sizeof(info)) ? 0 : -1;
}
REGISTER_SYSCALL(SYS_TTY_INFO, sys_tty_info, 2)

static int64_t sys_mount(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a6;
    char dev[64], mnt[64];
    
    if (!syscall_current_admin_allowed() ||
        !security_copy_from_user(dev, (const void*)a1, sizeof(dev)) ||
        !security_copy_from_user(mnt, (const void*)a2, sizeof(mnt)) ||
        a3 > FS_TYPE_ISO9660)
        return -1;

    dev[sizeof(dev) - 1] = '\0';
    mnt[sizeof(mnt) - 1] = '\0';

    return vfs_mount(dev, mnt, (fs_type_t)a3, (int)a4, (void*)a5);
}
REGISTER_SYSCALL(SYS_MOUNT, sys_mount, 5)

static int64_t sys_unmount(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    char mnt[64];
    
    if (!syscall_current_admin_allowed() ||
        !security_copy_from_user(mnt, (const void*)a1, sizeof(mnt)))
        return -1;
    mnt[sizeof(mnt) - 1] = '\0';
    if (!strcmp(mnt, "/")) return -1;
    return vfs_unmount(mnt);
}
REGISTER_SYSCALL(SYS_UNMOUNT, sys_unmount, 1)

static int64_t sys_sync(uint64_t a1, uint64_t a2, uint64_t a3,
                        uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    return vfs_sync() ? 0 : -1;
}
REGISTER_SYSCALL(SYS_SYNC, sys_sync, 0)

static int64_t sys_history_count(uint64_t a1, uint64_t a2, uint64_t a3,
                                 uint64_t a4, uint64_t a5, uint64_t a6)
{
    int console = syscall_current_console();
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    if (console < 0 || console >= TTY_CONSOLE_COUNT) return -1;
    return shell_history_count[console];
}
REGISTER_SYSCALL(SYS_HISTORY_COUNT, sys_history_count, 0)

static int64_t sys_history_get(uint64_t a1, uint64_t a2, uint64_t a3,
                               uint64_t a4, uint64_t a5, uint64_t a6)
{
    int console = syscall_current_console();
    char* output = (char*)a2;
    size_t length;
    (void)a4; (void)a5; (void)a6;
    if (console < 0 || console >= TTY_CONSOLE_COUNT ||
        a1 >= shell_history_count[console])
        return -1;
    length = strlen(shell_history[console][a1]) + 1;
    if (a3 < length || !security_valid_user_range(output, length) ||
        !security_copy_to_user(output, shell_history[console][a1], length))
        return -1;
    return (int64_t)(length - 1);
}
REGISTER_SYSCALL(SYS_HISTORY_GET, sys_history_get, 3)

static int64_t sys_history_add(uint64_t a1, uint64_t a2, uint64_t a3,
                               uint64_t a4, uint64_t a5, uint64_t a6)
{
    int console = syscall_current_console();
    char line[SHELL_HISTORY_LENGTH];
    uint32_t count;
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    if (console < 0 || console >= TTY_CONSOLE_COUNT ||
        !security_copy_user_string(line, sizeof(line), (const char*)a1,
                                   sizeof(line)) || !line[0])
        return -1;

    count = shell_history_count[console];
    if (count && !strcmp(shell_history[console][count - 1], line))
        return 0;
    if (count == SHELL_HISTORY_COUNT) {
        for (uint32_t i = 1; i < count; i++)
            memcpy(shell_history[console][i - 1],
                   shell_history[console][i], SHELL_HISTORY_LENGTH);
        count--;
    }
    strcpy(shell_history[console][count], line);
    shell_history_count[console] = count + 1;
    return 0;
}
REGISTER_SYSCALL(SYS_HISTORY_ADD, sys_history_add, 1)

/* ============================================================
 * INIT & DISPATCH
 * ============================================================ */

void syscall_init(void) {
    vga_print("[syscall] Initializing syscall table...\n", 0x0A);

    syscall_count = 0;
    memset(syscall_table, 0, sizeof(syscall_table));
    if (!syscall_file_creation_umask_initialized) {
        for (int console = 0; console < TTY_CONSOLE_COUNT; console++)
            syscall_file_creation_umask[console] = 0022;
        syscall_file_creation_umask_initialized = 1;
    }

    for (const syscall_reg_t* ptr = __start_syscalls; ptr < __stop_syscalls; ptr++) {
        if (ptr->handler && ptr->syscall_num < MAX_SYSCALLS) {
            syscall_table[ptr->syscall_num].handler = ptr->handler;
            syscall_table[ptr->syscall_num].num_args = ptr->num_args;
            if (ptr->syscall_num >= syscall_count) {
                syscall_count = ptr->syscall_num + 1;
            }

            vga_print("syscall #", 0x0F);
            vga_put_uint_auto(ptr->syscall_num, 0x0F);
            vga_print(": ", 0x0F);
            vga_print(ptr->name, 0x0F);
            vga_print(" (", 0x0F);
            vga_put_uint_auto(ptr->num_args, 0x0F);
            vga_print(" args)\n", 0x0F);
        }
    }

    vga_print("syscall Total: ", 0x0F);
    vga_put_uint_auto(syscall_count, 0x0F);
    vga_print(" syscalls registered\n", 0x0F);
}

int64_t syscall_dispatch(uint32_t num, uint64_t a1, uint64_t a2, uint64_t a3,
                         uint64_t a4, uint64_t a5, uint64_t a6) {
    if (num >= syscall_count) {
        return -1;
    }

    syscall_entry_t* entry = &syscall_table[num];
    if (!entry->handler) {
        return -1;
    }

    switch (entry->num_args) {
        case 0: return ((int64_t(*)(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t))entry->handler)(0,0,0,0,0,0);
        case 1: return ((int64_t(*)(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t))entry->handler)(a1,0,0,0,0,0);
        case 2: return ((int64_t(*)(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t))entry->handler)(a1,a2,0,0,0,0);
        case 3: return ((int64_t(*)(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t))entry->handler)(a1,a2,a3,0,0,0);
        case 4: return ((int64_t(*)(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t))entry->handler)(a1,a2,a3,a4,0,0);
        case 5: return ((int64_t(*)(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t))entry->handler)(a1,a2,a3,a4,a5,0);
        case 6: return ((int64_t(*)(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t))entry->handler)(a1,a2,a3,a4,a5,a6);
        default: return -1;
    }
}