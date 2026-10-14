// ============================================================
// CreateOS 0.0.1
// ============================================================

#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>
#include <stddef.h>

/* ============================================================
 * Syscall numbers (Linux-compatible where possible)
 * ============================================================ */
#define SYS_EXIT        0
#define SYS_PUTC        1
#define SYS_PUTS        2
#define SYS_GETC        3
#define SYS_PUTCHAR     4
#define SYS_CLEAR       5

#define SYS_OPEN       10
#define SYS_READ       11
#define SYS_WRITE      12
#define SYS_CLOSE      13
#define SYS_STAT       14
#define SYS_MKDIR      15
#define SYS_UNLINK     16
#define SYS_LIST       17
#define SYS_SEEK       18
#define SYS_CHDIR      19
#define SYS_CHMOD      20
#define SYS_CHOWN      21
#define SYS_CWD        22  /* getcwd */

#define SYS_FORK       30
#define SYS_EXEC       31
#define SYS_WAIT       32
#define SYS_GETPID     33
#define SYS_YIELD      34
#define SYS_KILL       35

#define SYS_USER_ADD       40
#define SYS_USER_LOGIN     41
#define SYS_USER_LOGOUT    42
#define SYS_USER_FIND      43
#define SYS_USER_SETPASS   44
#define SYS_USER_CURRENT   45
#define SYS_USER_LOCK      46
#define SYS_USER_UNLOCK    47
#define SYS_USER_DISABLE   48
#define SYS_USER_ENABLE    49
#define SYS_USER_COUNT     53
#define SYS_USER_SETROLE   54
#define SYS_USER_GET       55

#define SYS_TICKS      50
#define SYS_SLEEP      51
#define SYS_TIME       52

#define SYS_ALLOC      60
#define SYS_FREE       61

#define SYS_DEBUG      70

#define SYS_STATFS     71
#define SYS_TTY_NAME   72
#define SYS_TTY_ACTIVATE 73
#define SYS_BEEP       74
#define SYS_FONT_SET   75
#define SYS_FONT_GET   76
#define SYS_MOUNT      77
#define SYS_UNMOUNT    78
#define SYS_TTY_INFO   79
#define SYS_SYNC       80
#define SYS_HISTORY_COUNT 81
#define SYS_HISTORY_GET   82
#define SYS_HISTORY_ADD   83
#define SYS_FSCK          84
#define SYS_USB_INFO      85
#define SYS_FILE_REPLACE  86
#define SYS_READ_AT       87
#define SYS_FSTAT         88
#define SYS_GETDENTS64    89
#define SYS_TTY_SET_CURSOR 90
#define SYS_NET_COUNT     91
#define SYS_NET_INFO      92
#define SYS_NET_DHCP      93
#define SYS_NET_PING      94
#define SYS_KERNEL_VERSION 95
#define SYS_NET_TCP       96
#define SYS_NET_RESOLVE   97
#define SYS_TTY_FULLSCREEN_BEGIN 98
#define SYS_TTY_FULLSCREEN_END   99
#define SYS_TTY_MOUSE_POSITION  100
#define SYS_APP_RETURN_EXPLORER 101
#define SYS_NET_TCP_STREAM_OPEN 102
#define SYS_NET_TCP_STREAM_WRITE 103
#define SYS_NET_TCP_STREAM_READ 104
#define SYS_NET_TCP_STREAM_CLOSE 105
#define SYS_RANDOM      106
#define SYS_USER_VERIFY_PASSWORD 107
#define SYS_POWER_INFO 108
#define SYS_POWER_SET_IDLE 109
#define SYS_PCI_INFO 110
#define SYS_MONOTONIC_NS 111
#define SYS_TTY_POINTER_MODE 112
#define SYS_TTY_POLLCHAR 113
#define SYS_TTY_CURSOR_POSITION 114
#define SYS_CLIPBOARD_SET 115
#define SYS_CLIPBOARD_GET 116
#define SYS_TTY_SHELL_INPUT 117
#define SYS_FONT_COUNT 118
#define SYS_RENAME     119
#define SYS_NET_TCP_LISTEN 120
#define SYS_NET_TCP_ACCEPT 121
#define SYS_NET_TCP_LISTENER_CLOSE 122
#define SYS_TTY_CTRL_C_INTERRUPT 123
#define SYS_RUN 124
#define SYS_UMASK 125
#define SYS_ACCESS 126
#define SYS_MAX        128

typedef int64_t (*syscall_handler_t)(uint64_t a1, uint64_t a2, uint64_t a3,
                                      uint64_t a4, uint64_t a5, uint64_t a6);

typedef struct {
    syscall_handler_t handler;
    uint8_t num_args;
} syscall_entry_t;

void syscall_init(void);
void syscall_resume_init(void);
void syscall_process_transition(void);
#ifdef PROC_CONTEXT_SELFTEST
int syscall_test_process_handle_revocation(void);
#endif

int64_t syscall_dispatch(uint32_t num, uint64_t a1, uint64_t a2, uint64_t a3,
                         uint64_t a4, uint64_t a5, uint64_t a6);

#endif