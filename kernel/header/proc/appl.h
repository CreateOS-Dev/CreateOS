// ============================================================
// CreateOS 0.0.1
// ============================================================

#ifndef APPL_H
#define APPL_H

#include <stdint.h>
#include "memory/vmm.h"

#define APPL_MAGIC 0x4150504C
#define APPL_USER_IMAGE_BASE 0x0000000040000000ULL
#define APPL_USER_IMAGE_SIZE 0x20000000ULL
#define APPL_USER_STACK_BASE 0x000000007F800000ULL
#define APPL_USER_STACK_SIZE 0x00800000ULL
#define APPL_USER_STACK_TOP (APPL_USER_STACK_BASE + APPL_USER_STACK_SIZE)
#define APPL_USER_HEAP_BASE 0x0000000060000000ULL
#define APPL_USER_HEAP_SIZE 0x1F800000ULL
#define APPL_USER_HEAP_PAGE_COUNT (APPL_USER_HEAP_SIZE / 0x1000ULL)
#define APPL_USER_HEAP_ALLOCATION_MAX 128

typedef struct {
    uint32_t first_page;
    uint32_t page_count;
} appl_user_heap_allocation_t;

typedef struct {
    vmm_address_space_t address_space;
    uint64_t stack_top;
    int has_system_privileges;
    uint8_t heap_used[APPL_USER_HEAP_PAGE_COUNT];
    uint8_t heap_mapped[APPL_USER_HEAP_PAGE_COUNT];
    appl_user_heap_allocation_t
        heap_allocations[APPL_USER_HEAP_ALLOCATION_MAX];
} appl_user_fork_state_t;

struct appl_header {
    uint32_t magic;
    uint32_t total_size;
    uint32_t entry_offset;
    uint32_t reserved[5];
};

int appl_run_file_argv(const char* name);

int appl_launch_user(const char* name);

int user_exec(const char* path, char* const argv[]);
int user_exec_from_user(const char* path, char* const argv[]);
int appl_run_user_child(const char* path, char* const argv[]);
int appl_fork_user_state(appl_user_fork_state_t* state);
int appl_run_forked_user_child(appl_user_fork_state_t* state);
int appl_restore_user_parent(void);
int appl_user_child_depth(void);
int appl_path_has_system_privileges(const char* path);
int appl_exec_retains_system_privileges(int current_privileges,
                                        const char* path);
int appl_current_has_system_privileges(void);
void* appl_user_alloc(uint64_t size);
int appl_user_free(void* address);

int appl_kill(int pid);

#endif
