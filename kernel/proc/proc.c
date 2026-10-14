// ============================================================
// CreateOS 0.0.1
// ============================================================

#include "proc/proc.h"
#include "proc/appl.h"
#include "proc/user.h"
#include "drivers/vga.h"
#include "drivers/tty.h"
#include "drivers/serial.h"
#include "memory/pmm.h"
#include "lib/string.h"
#include "security/process.h"
#include "security/user_access.h"
#include "syscall.h"
#include <stddef.h>

#define PROC_STACK_SIZE 4096
#define PROC_INITIAL_RFLAGS 0x202

_Static_assert(offsetof(proc_context_t, rsp) == 48,
               "proc_context_t assembly layout changed");
_Static_assert(offsetof(proc_context_t, cr3) == 72,
               "proc_context_t assembly layout changed");
_Static_assert(sizeof(proc_context_t) == 80,
               "proc_context_t assembly layout changed");

static process_t g_proc[MAX_PROCESSES];
static proc_context_t boot_context;
static int g_current = -1;
static uint32_t g_next_pid = 1;

extern void proc_context_switch(proc_context_t* old_context,
                                const proc_context_t* new_context,
                                uint64_t old_rflags);

static uint64_t proc_read_rflags(void)
{
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0" : "=r"(flags));
    return flags;
}

static void proc_restore_rflags(uint64_t flags)
{
    __asm__ volatile("push %0; popfq" : : "r"(flags) : "memory", "cc");
}

static void proc_reap_zombies(void)
{
    for (int i = 0; i < MAX_PROCESSES; i++) {
        process_t* process = &g_proc[i];
        if (i == g_current || process->state != PROC_ZOMBIE)
            continue;

        vmm_address_space_destroy(&process->address_space);
        if (process->stack)
            pmm_free_frame(process->stack);
        memset(process, 0, sizeof(*process));
    }
}

static void proc_task_returned(void)
{
    proc_exit_current();
    for (;;) __asm__ volatile("cli; hlt");
}

#ifdef PROC_CONTEXT_SELFTEST
static volatile uint32_t proc_test_phase;
static volatile uint32_t proc_test_failed;
static uint64_t proc_test_boot_cr3;

static uint64_t proc_test_read_cr3(void)
{
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

static void proc_test_finish(uint32_t result)
{
    (void)result;
    for (;;) __asm__ volatile("cli; hlt");
}

static int proc_test_user_mapping_isolation(void)
{
    vmm_address_space_t space = { 0 };
    vmm_address_space_t clone = { 0 };
    uint64_t* frame = (uint64_t*)pmm_alloc_frame();
    uint64_t* cloned_frame;
    uint64_t kernel_frame;
    uint64_t mapped_frame;
    uint64_t* collision_frame = NULL;
    uint64_t saved_cr3;
    uint64_t free_frames_before_mapping;
    int frame_mapped = 0;
    int result = 0;

    if (!frame || !vmm_address_space_create(&space))
        goto done;
    free_frames_before_mapping = pmm_get_free_frames();
    if (vmm_address_space_page_flags(&space, VMM_USER_ADDRESS_BASE))
        goto done;
    kernel_frame = vmm_address_space_page_frame(
        &space, VMM_USER_ADDRESS_BASE - 0x1000000);
    if (!kernel_frame ||
        kernel_frame != (uint64_t)(uintptr_t)vmm_get_phys_addr(
            (void*)(uintptr_t)(VMM_USER_ADDRESS_BASE - 0x1000000)) ||
        (vmm_address_space_page_flags(
             &space, VMM_USER_ADDRESS_BASE - 0x1000000) & PAGE_USER))
        goto done;
    if (vmm_address_space_map(&space, VMM_USER_ADDRESS_BASE - 0x1000,
                              (uint64_t)(uintptr_t)frame,
                              PAGE_RW | PAGE_USER))
        goto done;
    if (vmm_address_space_map(&space, VMM_USER_ADDRESS_LIMIT,
                              (uint64_t)(uintptr_t)frame,
                              PAGE_RW | PAGE_USER))
        goto done;
    if (!vmm_address_space_map(&space, VMM_USER_ADDRESS_BASE,
                               (uint64_t)(uintptr_t)frame,
                               PAGE_RW | PAGE_USER |
                               (vmm_nx_enabled() ? PAGE_NX : 0)))
        goto done;
    frame_mapped = 1;
    if (vmm_address_space_page_frame(&space, VMM_USER_ADDRESS_BASE) !=
            (uint64_t)(uintptr_t)frame ||
        !(vmm_address_space_page_flags(&space, VMM_USER_ADDRESS_BASE) &
          PAGE_USER))
        goto done;
    collision_frame = (uint64_t*)pmm_alloc_frame();
    if (!collision_frame)
        goto done;
    if (vmm_address_space_map(&space, VMM_USER_ADDRESS_BASE,
                              (uint64_t)(uintptr_t)collision_frame,
                              PAGE_RW | PAGE_USER)) {
        if (vmm_address_space_unmap(&space, VMM_USER_ADDRESS_BASE,
                                    &mapped_frame)) {
            pmm_free_frame((void*)(uintptr_t)mapped_frame);
            collision_frame = NULL;
            frame_mapped = 0;
        }
        pmm_free_frame(frame);
        frame = NULL;
        goto done;
    }
    if (vmm_address_space_page_frame(&space, VMM_USER_ADDRESS_BASE) !=
        (uint64_t)(uintptr_t)frame)
        goto done;
    frame[0] = 0x123456789ABCDEF0ULL;
    if (!vmm_address_space_clone(&space, &clone))
        goto done;
    cloned_frame = (uint64_t*)(uintptr_t)
        vmm_address_space_page_frame(&clone, VMM_USER_ADDRESS_BASE);
    if (!cloned_frame ||
        (uint64_t)(uintptr_t)cloned_frame == (uint64_t)(uintptr_t)frame ||
        cloned_frame[0] != frame[0])
        goto done;
    cloned_frame[0] = 0x0FEDCBA987654321ULL;
    if (frame[0] != 0x123456789ABCDEF0ULL)
        goto done;
    vmm_address_space_destroy(&clone);
    pmm_free_frame(collision_frame);
    collision_frame = NULL;
    __asm__ volatile("mov %%cr3, %0" : "=r"(saved_cr3));
    vmm_address_space_activate(&space);
    if (!security_valid_user_range(
            (const void*)(uintptr_t)VMM_USER_ADDRESS_BASE, 1) ||
        !security_valid_user_write_range(
            (const void*)(uintptr_t)VMM_USER_ADDRESS_BASE, 1) ||
        security_valid_user_range(
            (const void*)(uintptr_t)(VMM_USER_ADDRESS_BASE - 0x1000), 1) ||
        security_valid_user_range(
            (const void*)(uintptr_t)(VMM_USER_ADDRESS_BASE + 0xFFF), 2) ||
        security_valid_user_range(
            (const void*)(uintptr_t)VMM_USER_ADDRESS_LIMIT, 1)) {
        __asm__ volatile("mov %0, %%cr3" :: "r"(saved_cr3) : "memory");
        goto done;
    }
    {
        char copied_string[4];
        char* user_string =
            (char*)(uintptr_t)(VMM_USER_ADDRESS_BASE + 0xFFD);
        user_string[0] = 'o';
        user_string[1] = 'k';
        user_string[2] = 0;
        if (!security_valid_user_string(user_string, 3) ||
            !security_copy_user_string(copied_string, sizeof(copied_string),
                                       user_string, 3) ||
            strcmp(copied_string, "ok") != 0) {
            __asm__ volatile("mov %0, %%cr3" :: "r"(saved_cr3) : "memory");
            goto done;
        }
        user_string[2] = 'x';
        if (security_valid_user_string(user_string, 4)) {
            __asm__ volatile("mov %0, %%cr3" :: "r"(saved_cr3) : "memory");
            goto done;
        }
    }
    __asm__ volatile("mov %0, %%cr3" :: "r"(saved_cr3) : "memory");
    if (!vmm_address_space_unmap(&space, VMM_USER_ADDRESS_BASE,
                                 &mapped_frame) ||
        mapped_frame != (uint64_t)(uintptr_t)frame ||
        pmm_get_free_frames() != free_frames_before_mapping)
        goto done;
    frame_mapped = 0;
    result = 1;

done:
    if (clone.pml4)
        vmm_address_space_destroy(&clone);
    if (collision_frame)
        pmm_free_frame(collision_frame);
    if (frame_mapped && space.pml4 &&
        vmm_address_space_unmap(&space, VMM_USER_ADDRESS_BASE, NULL))
        frame_mapped = 0;
    if (space.pml4)
        vmm_address_space_destroy(&space);
    if (frame && !frame_mapped)
        pmm_free_frame(frame);
    return result;
}

static int proc_test_unprivileged_syscall_policy(void)
{
    uint64_t generation = security_process_generation(0);
    uint64_t other_console_generation = security_process_generation(1);
    if (!generation || !security_advance_process_generation(0) ||
        security_process_generation(0) == generation ||
        security_process_generation(1) != other_console_generation ||
        security_process_generation_matches(0, generation) ||
        !security_process_generation_matches(
            0, security_process_generation(0)) ||
        security_process_generation_matches(0, 0) ||
        security_process_generation(-1) != 0 ||
        security_advance_process_generation(TTY_CONSOLE_COUNT))
        return 0;
    syscall_init();
    if (!syscall_test_process_handle_revocation() ||
        appl_current_has_system_privileges() ||
        !appl_path_has_system_privileges("/bin/sh") ||
        !appl_path_has_system_privileges("/sbin/init") ||
        !appl_path_has_system_privileges("/app/system/bin/settings.APPL") ||
        appl_exec_retains_system_privileges(
            0, "/app/system/bin/settings.APPL") ||
        !appl_exec_retains_system_privileges(1, "/bin/sh") ||
        appl_exec_retains_system_privileges(1, "/home/admin/app") ||
        appl_path_has_system_privileges("/bin-malware/evil") ||
        appl_path_has_system_privileges("/b") ||
        appl_path_has_system_privileges("/home/admin/evil"))
        return 0;
    if (syscall_dispatch(SYS_EXIT, 0xDEAD, 0, 0, 0, 0, 0) != -13)
        return 0;
    if (syscall_dispatch(SYS_EXIT, 0xBEEF, 0, 0, 0, 0, 0) != -13)
        return 0;
    if (syscall_dispatch(SYS_NET_PING, 0, 0, 1, 0, 0, 0) != -1)
        return 0;
    if (syscall_dispatch(SYS_NET_RESOLVE, 0, 0, 0, 0, 0, 0) != -1)
        return 0;
    if (syscall_dispatch(SYS_NET_TCP_STREAM_OPEN, 0, 0, 80, 1000,
                         0, 0) != -1)
        return 0;
    if (syscall_dispatch(SYS_USER_VERIFY_PASSWORD, 0, 0, 0, 0, 0, 0) != -13 ||
        kernel_user_verify_current_password("not-a-password") != 0 ||
        !kernel_user_test_login_rejects_active_identity())
        return 0;
    return 1;
}

static void proc_test_first_task(void)
{
    if (proc_test_phase != 0 ||
        proc_test_read_cr3() != g_proc[0].context.cr3)
        proc_test_failed = 1;
    proc_test_phase = 1;
    proc_yield();
    if (proc_test_phase != 2 ||
        proc_test_read_cr3() != g_proc[0].context.cr3)
        proc_test_failed = 1;
    proc_test_phase = 3;
}

static void proc_test_second_task(void)
{
    if (proc_test_phase != 1 ||
        proc_test_read_cr3() != g_proc[1].context.cr3 ||
        g_proc[0].context.cr3 == g_proc[1].context.cr3)
        proc_test_failed = 1;
    proc_test_phase = 2;
}

static void proc_context_selftest(void)
{
    proc_test_phase = 0;
    proc_test_failed = 0;
    proc_test_boot_cr3 = proc_test_read_cr3();
    if (!proc_test_user_mapping_isolation()) {
        serial_puts("[proc-test] FAIL: user mapping overlaps kernel identity map\n");
        proc_test_finish(1);
    }
    if (!proc_test_unprivileged_syscall_policy()) {
        serial_puts("[proc-test] FAIL: unauthenticated privileged syscall allowed\n");
        proc_test_finish(1);
    }
    if (proc_create("switch-test-a", proc_test_first_task) < 0 ||
        proc_create("switch-test-b", proc_test_second_task) < 0) {
        serial_puts("[proc-test] FAIL: task setup\n");
        proc_test_finish(1);
    }

    proc_schedule();
    proc_schedule();
    if (proc_test_failed || proc_test_phase != 3 ||
        proc_test_read_cr3() != proc_test_boot_cr3) {
        serial_puts("[proc-test] FAIL: context switch/order/address space\n");
        proc_test_finish(1);
    }
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (g_proc[i].state != PROC_UNUSED) {
            serial_puts("[proc-test] FAIL: task cleanup\n");
            proc_test_finish(1);
        }
    }
    serial_puts("[proc-test] PASS: context switch, mapping isolation, and syscall policy\n");
    proc_test_finish(0);
}
#endif

void proc_init(void)
{
    memset(g_proc, 0, sizeof(g_proc));
    memset(&boot_context, 0, sizeof(boot_context));
    g_current = -1;
    g_next_pid = 1;
#ifdef PROC_CONTEXT_SELFTEST
    proc_context_selftest();
#endif
}

int proc_create(const char* name, void (*entry)(void))
{
    if (!name || !entry || strlen(name) >= sizeof(g_proc[0].name))
        return -1;

    for (int i = 0; i < MAX_PROCESSES; i++) {
        process_t* process = &g_proc[i];
        if (process->state != PROC_UNUSED)
            continue;

        memset(process, 0, sizeof(*process));
        process->stack = (uint8_t*)pmm_alloc_frame();
        if (!process->stack)
            return -1;
        memset(process->stack, 0, PROC_STACK_SIZE);

        if (!vmm_address_space_create(&process->address_space)) {
            pmm_free_frame(process->stack);
            memset(process, 0, sizeof(*process));
            return -1;
        }

        uint64_t cr3 =
            (uint64_t)vmm_get_phys_addr(process->address_space.pml4);
        if (!cr3) {
            vmm_address_space_destroy(&process->address_space);
            pmm_free_frame(process->stack);
            memset(process, 0, sizeof(*process));
            return -1;
        }

        process->pid = (int)g_next_pid++;
        if (g_next_pid > INT32_MAX)
            g_next_pid = 1;
        process->state = PROC_READY;
        strcpy(process->name, name);
        process->stack_top =
            (uint64_t)(uintptr_t)process->stack + PROC_STACK_SIZE;

        uintptr_t initial_rsp = (uintptr_t)process->stack_top -
                                sizeof(uintptr_t);
        *(uintptr_t*)initial_rsp = (uintptr_t)proc_task_returned;
        process->context.rsp = initial_rsp;
        process->context.rip = (uint64_t)(uintptr_t)entry;
        process->context.rflags = PROC_INITIAL_RFLAGS;
        process->context.cr3 = cr3;
        return process->pid;
    }

    return -1;
}

void proc_kill(int pid)
{
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (g_proc[i].pid != pid || g_proc[i].state == PROC_UNUSED ||
            g_proc[i].state == PROC_ZOMBIE)
            continue;
        g_proc[i].state = PROC_ZOMBIE;
        if (i == g_current) {
            proc_schedule();
            for (;;) __asm__ volatile("cli; hlt");
        }
        return;
    }
}

void proc_exit_current(void)
{
    if (g_current < 0)
        return;
    proc_kill(g_proc[g_current].pid);
}

void proc_schedule(void)
{
    uint64_t flags = proc_read_rflags();
    __asm__ volatile("cli" ::: "memory");

    proc_reap_zombies();

    int old = g_current;
    if (old >= 0 && g_proc[old].state == PROC_RUNNING)
        g_proc[old].state = PROC_READY;

    int next = -1;
    for (int offset = 1; offset <= MAX_PROCESSES; offset++) {
        int index = old < 0 ? offset - 1 :
                    (old + offset) % MAX_PROCESSES;
        if (index == old || g_proc[index].state != PROC_READY)
            continue;
        next = index;
        break;
    }

    if (next < 0) {
        if (old >= 0) {
            g_current = -1;
            proc_context_switch(&g_proc[old].context, &boot_context, flags);
            return;
        }
        proc_restore_rflags(flags);
        return;
    }

    g_current = next;
    g_proc[next].state = PROC_RUNNING;
    proc_context_switch(old < 0 ? &boot_context : &g_proc[old].context,
                        &g_proc[next].context, flags);
}

void proc_tick(void)
{
    /* IRQ preemption remains disabled until interrupt frames can be switched. */
}

void proc_dump(void)
{
    vga_print("PID   STATE   NAME\n", 0x0F);
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (g_proc[i].state == PROC_UNUSED)
            continue;

        vga_put_uint_auto(g_proc[i].pid, 0x0E);
        vga_print("   ", 0x0F);
        switch (g_proc[i].state) {
            case PROC_READY:   vga_print("READY   ", 0x0F); break;
            case PROC_RUNNING: vga_print("RUNNING ", 0x0F); break;
            case PROC_ZOMBIE:  vga_print("ZOMBIE  ", 0x0F); break;
            case PROC_PAUSED:  vga_print("PAUSED  ", 0x0F); break;
            default:           vga_print("???     ", 0x0F); break;
        }
        vga_print(g_proc[i].name, 0x0F);
        vga_print("\n", 0x0F);
    }
}

void proc_yield(void)
{
    proc_schedule();
}
