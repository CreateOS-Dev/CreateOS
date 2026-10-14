// ============================================================
// CreateOS 0.0.1
// ============================================================

#ifndef PROC_H
#define PROC_H

#include <stdint.h>
#include "memory/vmm.h"

#define MAX_PROCESSES 32

typedef enum {
    PROC_UNUSED = 0,
    PROC_READY,
    PROC_RUNNING,
    PROC_ZOMBIE,
    PROC_PAUSED
} proc_state_t;

typedef struct {
    uint64_t rbx;
    uint64_t rbp;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
    uint64_t rsp;
    uint64_t rip;
    uint64_t rflags;
    uint64_t cr3;
} proc_context_t;

typedef struct process {
    int pid;
    proc_state_t state;
    char name[32];

    uint8_t* stack;
    uint64_t stack_top;
    vmm_address_space_t address_space;

    proc_context_t context;
} process_t;

void proc_init(void);
/* Creates a cooperative kernel task; this is not a ring-3 process API. */
int  proc_create(const char* name, void (*entry)(void));
void proc_kill(int pid);
void proc_exit_current(void);
void proc_schedule(void);
void proc_tick(void);
void proc_dump(void);
/* Must only be called from a cooperative kernel task. */
void proc_yield(void);

#endif
