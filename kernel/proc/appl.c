// ============================================================
// CreateOS 0.0.1
// ============================================================

#include <stdint.h>
#include <stddef.h>
#include "drivers/vga.h"
#include "drivers/tty.h"
#include "fs/fs.h"
#include "proc/appl.h"
#include "syscall.h"
#include "lib/string.h"
#include "drivers/serial.h"
#include "drivers/framebuffer.h"
#include "memory/pmm.h"
#include "memory/vmm.h"
#include "security/user_access.h"

#define APP_LOAD_ADDR APPL_USER_IMAGE_BASE
#define APP_IMAGE_LIMIT APPL_USER_IMAGE_SIZE
#define APP_ADDRESS_END (APP_LOAD_ADDR + APP_IMAGE_LIMIT)
#define APP_PAGE_SIZE 0x1000
#define APP_IMAGE_PAGE_COUNT (APP_IMAGE_LIMIT / APP_PAGE_SIZE)
#define USER_STACK_BASE APPL_USER_STACK_BASE
#define USER_STACK_TOP APPL_USER_STACK_TOP
#define USER_STACK_PAGES (APPL_USER_STACK_SIZE / APP_PAGE_SIZE)
#define USER_ARG_MAX 32
#define USER_ARG_LENGTH 256
#define USER_HEAP_PAGE_COUNT (APPL_USER_HEAP_SIZE / APP_PAGE_SIZE)
#define USER_HEAP_ALLOCATION_MAX APPL_USER_HEAP_ALLOCATION_MAX
#define USER_CHILD_MAX 8
#define ELF_IO_BUFFER_SIZE (16 * 1024)
#define ELF_PT_LOAD     1
#define ELF_P_EXEC      1
#define ELF_P_WRITE     2
#define ELF_PAGE_PRESENT 0x01
#define ELF_PAGE_WRITE   0x02
#define ELF_PAGE_EXEC    0x04

#define USER_CS 0x23
#define USER_SS 0x1B

static uint64_t user_stack_top;
static vmm_address_space_t active_user_space;
static int active_app_has_system_privileges;
static uint8_t* app_image_frames[APP_IMAGE_PAGE_COUNT];
static uint64_t app_image_size;
static uint8_t app_page_permissions[APP_IMAGE_PAGE_COUNT];
static uint8_t elf_io_buffer[ELF_IO_BUFFER_SIZE];
static uint8_t user_heap_used[USER_HEAP_PAGE_COUNT];
static uint8_t user_heap_mapped[USER_HEAP_PAGE_COUNT];
typedef struct {
    uint32_t first_page;
    uint32_t page_count;
} user_heap_allocation_t;
_Static_assert(sizeof(user_heap_allocation_t) ==
               sizeof(appl_user_heap_allocation_t),
               "user heap allocation ABI changed");
static user_heap_allocation_t user_heap_allocations[USER_HEAP_ALLOCATION_MAX];
static char user_exec_args[USER_ARG_MAX][USER_ARG_LENGTH];
typedef struct {
    vmm_address_space_t address_space;
    uint64_t stack_top;
    int has_system_privileges;
    uint8_t heap_used[USER_HEAP_PAGE_COUNT];
    uint8_t heap_mapped[USER_HEAP_PAGE_COUNT];
    user_heap_allocation_t heap_allocations[USER_HEAP_ALLOCATION_MAX];
} user_parent_state_t;
static user_parent_state_t user_parent_states[USER_CHILD_MAX];
static int user_child_depth;
extern uint64_t pml4_table[512];

int appl_path_has_system_privileges(const char* path)
{
    static const char* const system_directories[] = {
        "/bin", "/sbin", "/usr/bin", "/app/system/bin"
    };
    size_t path_length;

    if (!path) return 0;
    path_length = strlen(path);
    for (size_t i = 0;
         i < sizeof(system_directories) / sizeof(system_directories[0]); i++) {
        size_t length = strlen(system_directories[i]);
        if (path_length >= length &&
            strncmp(path, system_directories[i], length) == 0 &&
            (path[length] == 0 || path[length] == '/'))
            return 1;
    }
    return 0;
}

int appl_exec_retains_system_privileges(int current_privileges,
                                        const char* path)
{
    return current_privileges && appl_path_has_system_privileges(path);
}

int appl_current_has_system_privileges(void)
{
    return active_app_has_system_privileges;
}

static uint64_t user_image_page_flags(uint32_t page)
{
    uint8_t permissions = app_page_permissions[page];
    uint64_t flags = PAGE_USER;

    if (permissions & ELF_PAGE_WRITE)
        flags |= PAGE_RW;
    if (!(permissions & ELF_PAGE_EXEC) && vmm_nx_enabled())
        flags |= PAGE_NX;
    return flags;
}

static void* user_heap_address(uint32_t page)
{
    return (void*)(uintptr_t)(APPL_USER_HEAP_BASE +
                              (uint64_t)page * APP_PAGE_SIZE);
}

static void release_loaded_elf(void)
{
    for (uint32_t page = 0; page < APP_IMAGE_PAGE_COUNT; page++) {
        if (app_image_frames[page]) {
            pmm_free_frame(app_image_frames[page]);
            app_image_frames[page] = NULL;
        }
    }
    memset(app_page_permissions, 0, sizeof(app_page_permissions));
    app_image_size = 0;
}

static int user_heap_page_reserved(uint32_t page)
{
    for (uint32_t i = 0; i < USER_HEAP_ALLOCATION_MAX; i++) {
        const user_heap_allocation_t* allocation = &user_heap_allocations[i];
        if (allocation->page_count &&
            page >= allocation->first_page &&
            page - allocation->first_page < allocation->page_count)
            return 1;
    }
    return 0;
}

static int user_heap_release(uint32_t allocation_index)
{
    user_heap_allocation_t* allocation =
        &user_heap_allocations[allocation_index];

    if (!allocation->page_count) return 1;
    if (allocation->first_page >= USER_HEAP_PAGE_COUNT ||
        allocation->page_count >
            USER_HEAP_PAGE_COUNT - allocation->first_page)
        return 0;
    for (uint32_t i = 0; i < allocation->page_count; i++) {
        uint32_t page = allocation->first_page + i;
        if (!user_heap_mapped[page]) {
            if (user_heap_used[page]) return 0;
            continue;
        }
        uint64_t address = APPL_USER_HEAP_BASE +
            (uint64_t)page * APP_PAGE_SIZE;
        if (!vmm_address_space_page_frame(&active_user_space, address) ||
            !user_heap_used[page] ||
            !(vmm_address_space_page_flags(&active_user_space, address) &
              PAGE_USER))
            return 0;
    }
    for (uint32_t i = 0; i < allocation->page_count; i++) {
        uint32_t page = allocation->first_page + i;
        if (!user_heap_mapped[page]) continue;
        uint64_t address = APPL_USER_HEAP_BASE +
            (uint64_t)page * APP_PAGE_SIZE;
        uint64_t frame;
        if (!vmm_address_space_unmap(&active_user_space, address, &frame)) {
            serial_puts("[appl] ERROR: user heap unmap failed; allocation retained\n");
            return 0;
        }
        pmm_free_frame((void*)(uintptr_t)frame);
        user_heap_mapped[page] = 0;
        user_heap_used[page] = 0;
    }
    allocation->first_page = 0;
    allocation->page_count = 0;
    return 1;
}

void* appl_user_alloc(uint64_t size)
{
    uint32_t page_count;
    uint32_t allocation_index = USER_HEAP_ALLOCATION_MAX;
    uint32_t first_page = 0;
    uint32_t run_length = 0;

    if (!active_user_space.pml4 || !size || size > APPL_USER_HEAP_SIZE)
        return NULL;
    page_count = (uint32_t)((size + APP_PAGE_SIZE - 1) / APP_PAGE_SIZE);

    for (uint32_t i = 0; i < USER_HEAP_ALLOCATION_MAX; i++) {
        if (!user_heap_allocations[i].page_count) {
            allocation_index = i;
            break;
        }
    }
    if (allocation_index == USER_HEAP_ALLOCATION_MAX) return NULL;

    for (uint32_t page = 0; page < USER_HEAP_PAGE_COUNT; page++) {
        if (user_heap_used[page] || user_heap_page_reserved(page)) {
            run_length = 0;
            continue;
        }
        if (!run_length) first_page = page;
        if (++run_length == page_count) break;
    }
    if (run_length != page_count) return NULL;

    uint32_t mapped = 0;
    for (; mapped < page_count; mapped++) {
        void* frame = pmm_alloc_frame();
        uint64_t address = APPL_USER_HEAP_BASE +
            (uint64_t)(first_page + mapped) * APP_PAGE_SIZE;
        if (!frame) break;
        memset(frame, 0, APP_PAGE_SIZE);
        if (!vmm_address_space_map(&active_user_space, address,
                                   (uint64_t)(uintptr_t)frame,
                                   PAGE_RW | PAGE_USER |
                                   (vmm_nx_enabled() ? PAGE_NX : 0))) {
            pmm_free_frame(frame);
            break;
        }
        user_heap_used[first_page + mapped] = 1;
        user_heap_mapped[first_page + mapped] = 1;
    }

    if (mapped != page_count) {
        int cleanup_incomplete = 0;
        while (mapped) {
            uint64_t frame;
            uint32_t page = first_page + --mapped;
            uint64_t address = APPL_USER_HEAP_BASE +
                (uint64_t)page * APP_PAGE_SIZE;
            if (!vmm_address_space_unmap(&active_user_space, address, &frame)) {
                cleanup_incomplete = 1;
                continue;
            }
            pmm_free_frame((void*)(uintptr_t)frame);
            user_heap_mapped[page] = 0;
            user_heap_used[page] = 0;
        }
        if (cleanup_incomplete) {
            serial_puts("[appl] ERROR: incomplete user heap allocation rollback\n");
            user_heap_allocations[allocation_index].first_page = first_page;
            user_heap_allocations[allocation_index].page_count = page_count;
        }
        return NULL;
    }

    user_heap_allocations[allocation_index].first_page = first_page;
    user_heap_allocations[allocation_index].page_count = page_count;
    return user_heap_address(first_page);
}

int appl_user_free(void* address)
{
    uintptr_t value = (uintptr_t)address;
    if (value < APPL_USER_HEAP_BASE ||
        value >= APPL_USER_HEAP_BASE + APPL_USER_HEAP_SIZE ||
        ((value - APPL_USER_HEAP_BASE) & (APP_PAGE_SIZE - 1)))
        return 0;

    uint32_t first_page =
        (uint32_t)((value - APPL_USER_HEAP_BASE) / APP_PAGE_SIZE);
    for (uint32_t i = 0; i < USER_HEAP_ALLOCATION_MAX; i++) {
        if (user_heap_allocations[i].page_count &&
            user_heap_allocations[i].first_page == first_page)
            return user_heap_release(i);
    }
    return 0;
}

static int user_stack_copy(vmm_address_space_t* space, uint64_t address,
                           const void* source, size_t size)
{
    const uint8_t* bytes = (const uint8_t*)source;
    if (address < USER_STACK_BASE || address > USER_STACK_TOP ||
        size > USER_STACK_TOP - address)
        return 0;
    while (size) {
        uint64_t offset = address - USER_STACK_BASE;
        uint32_t page = (uint32_t)(offset / 0x1000);
        uint32_t page_offset = (uint32_t)(offset & 0xFFF);
        size_t chunk = 0x1000 - page_offset;
        uint64_t frame;
        if (chunk > size) chunk = size;
        if (page >= USER_STACK_PAGES) return 0;
        frame = vmm_address_space_page_frame(
            space, USER_STACK_BASE + (uint64_t)page * APP_PAGE_SIZE);
        if (!frame) return 0;
        memcpy((void*)(uintptr_t)(frame + page_offset), bytes, chunk);
        address += chunk;
        bytes += chunk;
        size -= chunk;
    }
    return 1;
}

static int user_stack_build_argv(vmm_address_space_t* space, int argc,
                                 const char arguments[USER_ARG_MAX][USER_ARG_LENGTH])
{
    uint64_t argument_addresses[USER_ARG_MAX];
    uint64_t initial_stack[USER_ARG_MAX + 2];
    uint64_t stack_pointer = USER_STACK_TOP;
    size_t table_size;

    if (argc < 0 || argc >= USER_ARG_MAX) return 0;
    for (int i = argc - 1; i >= 0; i--) {
        size_t length = strlen(arguments[i]) + 1;
        if (length > USER_ARG_LENGTH || length > stack_pointer - USER_STACK_BASE)
            return 0;
        stack_pointer -= length;
        if (!user_stack_copy(space, stack_pointer, arguments[i], length))
            return 0;
        argument_addresses[i] = stack_pointer;
    }

    stack_pointer &= ~0xFULL;
    initial_stack[0] = (uint64_t)argc;
    for (int i = 0; i < argc; i++) initial_stack[i + 1] = argument_addresses[i];
    initial_stack[argc + 1] = 0;
    table_size = (size_t)(argc + 2) * sizeof(uint64_t);
    if (table_size > stack_pointer - USER_STACK_BASE) return 0;
    stack_pointer = (stack_pointer - table_size) & ~0xFULL;
    if (!user_stack_copy(space, stack_pointer, initial_stack, table_size))
        return 0;
    user_stack_top = stack_pointer;
    return 1;
}

static int user_copy_argv(char* const argv[], int* argc_out)
{
    if (!argc_out) return 0;
    *argc_out = 0;
    if (!argv) return 1;
    for (int i = 0; i < USER_ARG_MAX; i++) {
        const char* argument;
        if (!security_copy_from_user(&argument, argv + i, sizeof(argument)))
            return 0;
        if (!argument) {
            *argc_out = i;
            return 1;
        }
        if (!security_copy_user_string(user_exec_args[i], USER_ARG_LENGTH,
                                       argument, USER_ARG_LENGTH))
            return 0;
    }
    return 0;
}

static void serial_hex_qword(uint64_t value)
{
    for (int shift = 60; shift >= 0; shift -= 4)
        serial_putc("0123456789abcdef"[(value >> shift) & 0x0F]);
}

static int install_user_address_space(int activate, int argc,
                                      const char arguments[USER_ARG_MAX][USER_ARG_LENGTH],
                                      vmm_address_space_t* space)
{
    if (!space || !app_image_size) return 0;
    serial_puts("[appl] Creating user page tables\n");
    if (!vmm_address_space_create(space)) {
        serial_puts("[appl] ERROR: unable to clone kernel page tables\n");
        goto fail;
    }
    serial_puts("[appl] User page tables ready\n");

    for (uint64_t address = APP_LOAD_ADDR;
         address < APP_LOAD_ADDR + app_image_size;
         address += APP_PAGE_SIZE) {
        uint32_t page = (uint32_t)((address - APP_LOAD_ADDR) / APP_PAGE_SIZE);
        if (!(app_page_permissions[page] & ELF_PAGE_PRESENT)) continue;
        uint8_t* frame = app_image_frames[page];
        if (!frame) {
            serial_puts("[appl] ERROR: executable image page is missing\n");
            goto fail;
        }
        if (!vmm_address_space_map(space, address, (uint64_t)frame,
                                   user_image_page_flags(page))) {
            serial_puts("[appl] ERROR: unable to map user image page\n");
            goto fail;
        }
        app_image_frames[page] = NULL;
    }
    serial_puts("[appl] User image mapped\n");

    for (uint64_t address = USER_STACK_BASE;
         address < USER_STACK_TOP; address += 0x1000) {
        uint8_t* frame = (uint8_t*)pmm_alloc_frame();
        if (!frame || !vmm_address_space_map(space, address,
                                              (uint64_t)frame,
                                              PAGE_RW | PAGE_USER |
                                              (vmm_nx_enabled() ? PAGE_NX : 0))) {
            serial_puts("[appl] ERROR: unable to map user stack page\n");
            if (frame) pmm_free_frame(frame);
            goto fail;
        }
        memset(frame, 0, 0x1000);
    }

    if (!user_stack_build_argv(space, argc, arguments)) {
        serial_puts("[appl] ERROR: arguments do not fit the user stack\n");
        goto fail;
    }

    serial_puts("[appl] User stack mapped\n");
    if (activate) {
        serial_puts("[appl] Activating user address space\n");
        vmm_address_space_activate(space);
    }
    return 1;

fail:
    vmm_address_space_destroy(space);
    return 0;
}

static int refresh_active_user_image(
    int argc, const char arguments[USER_ARG_MAX][USER_ARG_LENGTH],
    uint64_t* next_cr3)
{
    vmm_address_space_t staged_space = { 0 };
    vmm_address_space_t old_space;
    uint64_t previous_cr3;
    uint64_t kernel_cr3 = (uint64_t)pml4_table;
    uint64_t staged_cr3;
    uint64_t previous_stack_top = user_stack_top;

    if (!active_user_space.pml4 || !next_cr3) return 0;
    *next_cr3 = 0;

    // User mappings overlap the kernel's low identity map, so stage the new
    // address space while the kernel page tables are active.
    __asm__ volatile("mov %%cr3, %0" : "=r"(previous_cr3));
    __asm__ volatile("mov %0, %%cr3" :: "r"(kernel_cr3) : "memory");

    if (!install_user_address_space(0, argc, arguments, &staged_space)) {
        user_stack_top = previous_stack_top;
        __asm__ volatile("mov %0, %%cr3" :: "r"(previous_cr3) : "memory");
        return 0;
    }

    staged_cr3 = (uint64_t)vmm_get_phys_addr(staged_space.pml4);
    if (!staged_cr3) {
        serial_puts("[sys_exec] unable to resolve staged page tables\n");
        vmm_address_space_destroy(&staged_space);
        user_stack_top = previous_stack_top;
        __asm__ volatile("mov %0, %%cr3" :: "r"(previous_cr3) : "memory");
        return 0;
    }

    old_space = active_user_space;
    active_user_space = staged_space;
    memset(user_heap_used, 0, sizeof(user_heap_used));
    memset(user_heap_mapped, 0, sizeof(user_heap_mapped));
    memset(user_heap_allocations, 0, sizeof(user_heap_allocations));
    vmm_address_space_destroy(&old_space);
    *next_cr3 = staged_cr3;
    __asm__ volatile("mov %0, %%cr3" :: "r"(staged_cr3) : "memory");
    return 1;
}

extern void syscall_entry(void);

volatile uint64_t pending_user_rip = 0;
volatile uint64_t pending_user_rsp = 0;
volatile uint64_t pending_user_cr3 = 0;

struct elf64_ehdr {
    uint8_t e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

struct elf64_phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
};

static int load_elf(const char* name, uint64_t* out_entry)
{
    fs_stat_t status;
    struct elf64_ehdr ehdr;
    struct elf64_phdr phdr[32];
    uint64_t image_end = APP_LOAD_ADDR;
    uint64_t entry_address;
    int entry_executable = 0;

    release_loaded_elf();
    if (!name || !out_entry || !fs_stat(name, &status) ||
        status.type != CFS5_INODE_FILE || !status.size) {
        serial_puts("[appl] ERROR: executable is missing or empty\n");
        return 0;
    }
    if (status.size < sizeof(struct elf64_ehdr)) {
        serial_puts("[appl] ERROR: ELF header truncated\n");
        return 0;
    }
    if (!fs_read(name, (uint8_t*)&ehdr, sizeof(ehdr), 0)) {
        serial_puts("[appl] ERROR: unable to read ELF header\n");
        return 0;
    }
    if (ehdr.e_ident[0] != 0x7F || ehdr.e_ident[1] != 'E' ||
        ehdr.e_ident[2] != 'L' || ehdr.e_ident[3] != 'F' ||
        ehdr.e_ident[4] != 2 || ehdr.e_ident[5] != 1 ||
        ehdr.e_ident[6] != 1) {
        serial_puts("[appl] ERROR: invalid ELF magic\n");
        return 0;
    }
    if (ehdr.e_machine != 0x3E || ehdr.e_type != 2 ||
        ehdr.e_version != 1 || ehdr.e_ehsize != sizeof(ehdr) ||
        ehdr.e_phoff < ehdr.e_ehsize ||
        ehdr.e_phentsize != sizeof(struct elf64_phdr) || !ehdr.e_phnum ||
        ehdr.e_phnum > 32 || ehdr.e_phoff > status.size ||
        ehdr.e_phnum >
            (status.size - ehdr.e_phoff) / sizeof(struct elf64_phdr)) {
        serial_puts("[appl] ERROR: invalid ELF program-header table\n");
        return 0;
    }
    if (!fs_read(name, (uint8_t*)phdr,
                 ehdr.e_phnum * sizeof(struct elf64_phdr),
                 (uint32_t)ehdr.e_phoff)) {
        serial_puts("[appl] ERROR: unable to read ELF program headers\n");
        return 0;
    }

    for (uint32_t i = 0; i < ehdr.e_phnum; i++) {
        if (phdr[i].p_type != ELF_PT_LOAD) continue;
        if (phdr[i].p_flags & ~7U ||
            phdr[i].p_filesz > phdr[i].p_memsz ||
            phdr[i].p_offset > status.size ||
            phdr[i].p_filesz > status.size - phdr[i].p_offset ||
            (phdr[i].p_align > 1 &&
             ((phdr[i].p_align & (phdr[i].p_align - 1)) ||
              phdr[i].p_vaddr % phdr[i].p_align !=
                  (phdr[i].p_filesz ?
                       phdr[i].p_offset % phdr[i].p_align : 0)))) {
            serial_puts("[appl] ERROR: malformed ELF load segment\n");
            return 0;
        }
        if (!phdr[i].p_memsz) continue;
        if (phdr[i].p_vaddr < APP_LOAD_ADDR ||
            phdr[i].p_vaddr >= APP_ADDRESS_END ||
            phdr[i].p_memsz > APP_ADDRESS_END - phdr[i].p_vaddr) {
            serial_puts("[appl] ERROR: ELF load segment is outside user image\n");
            return 0;
        }

        uint64_t segment_end = phdr[i].p_vaddr + phdr[i].p_memsz;
        for (uint32_t previous = 0; previous < i; previous++) {
            uint64_t previous_end;
            if (phdr[previous].p_type != ELF_PT_LOAD ||
                !phdr[previous].p_memsz)
                continue;
            previous_end = phdr[previous].p_vaddr +
                           phdr[previous].p_memsz;
            if (phdr[i].p_vaddr < previous_end &&
                phdr[previous].p_vaddr < segment_end) {
                serial_puts("[appl] ERROR: overlapping ELF load segments\n");
                return 0;
            }
        }
        if (segment_end > image_end) image_end = segment_end;
        if (ehdr.e_entry >= phdr[i].p_vaddr &&
            ehdr.e_entry < phdr[i].p_vaddr + phdr[i].p_filesz &&
            (phdr[i].p_flags & ELF_P_EXEC))
            entry_executable = 1;
    }

    if (image_end == APP_LOAD_ADDR ||
        ehdr.e_entry < APP_LOAD_ADDR || ehdr.e_entry >= image_end ||
        !entry_executable) {
        serial_puts("[appl] ERROR: ELF entry is not executable\n");
        return 0;
    }

    app_image_size = (image_end - APP_LOAD_ADDR + APP_PAGE_SIZE - 1) &
                     ~(uint64_t)(APP_PAGE_SIZE - 1);
    for (uint32_t i = 0; i < ehdr.e_phnum; i++) {
        if (phdr[i].p_type != ELF_PT_LOAD || !phdr[i].p_memsz) continue;
        uint32_t first_page = (uint32_t)(
            (phdr[i].p_vaddr - APP_LOAD_ADDR) / APP_PAGE_SIZE);
        uint32_t last_page = (uint32_t)(
            (phdr[i].p_vaddr + phdr[i].p_memsz - 1 - APP_LOAD_ADDR) /
            APP_PAGE_SIZE);
        uint8_t segment_permissions = ELF_PAGE_PRESENT;
        if (phdr[i].p_flags & ELF_P_WRITE)
            segment_permissions |= ELF_PAGE_WRITE;
        if (phdr[i].p_flags & ELF_P_EXEC)
            segment_permissions |= ELF_PAGE_EXEC;
        for (uint32_t page = first_page; page <= last_page; page++)
            app_page_permissions[page] |= segment_permissions;
    }

    for (uint32_t page = 0; page < app_image_size / APP_PAGE_SIZE; page++) {
        if ((app_page_permissions[page] &
             (ELF_PAGE_WRITE | ELF_PAGE_EXEC)) ==
            (ELF_PAGE_WRITE | ELF_PAGE_EXEC)) {
            serial_puts("[appl] ERROR: ELF page is writable and executable\n");
            release_loaded_elf();
            return 0;
        }
        if (!(app_page_permissions[page] & ELF_PAGE_PRESENT))
            continue;
        app_image_frames[page] = pmm_alloc_frame();
        if (!app_image_frames[page]) {
            serial_puts("[appl] ERROR: insufficient physical memory for ELF image\n");
            release_loaded_elf();
            return 0;
        }
        memset(app_image_frames[page], 0, APP_PAGE_SIZE);
    }

    for (uint32_t i = 0; i < ehdr.e_phnum; i++) {
        uint64_t copied = 0;
        if (phdr[i].p_type != ELF_PT_LOAD || !phdr[i].p_filesz) continue;
        while (copied < phdr[i].p_filesz) {
            uint32_t chunk = (uint32_t)(phdr[i].p_filesz - copied);
            uint32_t consumed = 0;
            if (chunk > sizeof(elf_io_buffer))
                chunk = sizeof(elf_io_buffer);
            if (!fs_read(name, elf_io_buffer, chunk,
                         (uint32_t)(phdr[i].p_offset + copied))) {
                serial_puts("[appl] ERROR: unable to read ELF segment data\n");
                release_loaded_elf();
                return 0;
            }
            while (consumed < chunk) {
                uint64_t virtual_address =
                    phdr[i].p_vaddr + copied + consumed;
                uint32_t page = (uint32_t)(
                    (virtual_address - APP_LOAD_ADDR) / APP_PAGE_SIZE);
                uint32_t page_offset = (uint32_t)(virtual_address &
                                                   (APP_PAGE_SIZE - 1));
                uint32_t page_chunk = APP_PAGE_SIZE - page_offset;
                if (page_chunk > chunk - consumed)
                    page_chunk = chunk - consumed;
                if (!app_image_frames[page]) {
                    serial_puts("[appl] ERROR: missing ELF segment page\n");
                    release_loaded_elf();
                    return 0;
                }
                memcpy(app_image_frames[page] + page_offset,
                       elf_io_buffer + consumed, page_chunk);
                consumed += page_chunk;
            }
            copied += (uint64_t)chunk;
        }
    }

    entry_address = ehdr.e_entry;
    serial_puts("[appl] ELF validation complete\n");
    *out_entry = entry_address;
    return 1;
}

extern void sysret_to_user(uint64_t rip, uint64_t rflags, uint64_t rsp,
                           uint64_t cr3);

int appl_launch_user(const char* name) {
    if (!name) {
        return 0;
    }
    tty_bind_execution_console();
    if (!vmm_nx_enabled()) {
        serial_puts("[appl] ERROR: refusing userspace without NX support\n");
        return 0;
    }

    serial_puts("[appl] Loading user program: ");
    serial_puts(name);
    serial_puts("\n");

    uint64_t entry = 0;
    if (!load_elf(name, &entry)) {
        vga_print("[appl] Failed to load ELF ", 0x0C);
        vga_print(name, 0x0C);
        vga_print("\n", 0x0C);
        serial_puts("[appl] Refusing to enter user mode\n");
        return 0;
    }

    if (!install_user_address_space(1, 0, NULL, &active_user_space)) {
        release_loaded_elf();
        serial_puts("[appl] Failed to create user address space\n");
        return 0;
    }
    release_loaded_elf();

    vga_print("[appl] Switching to user mode...\n", 0x0A);

    // RFLAGS = 0x202 (Interrupts Enable + Bit 1 réservé)
    uint64_t user_cr3 = (uint64_t)vmm_get_phys_addr(active_user_space.pml4);
    if (!user_cr3) {
        serial_puts("[appl] Failed to resolve user page-table address\n");
        vmm_address_space_destroy(&active_user_space);
        return 0;
    }
    active_app_has_system_privileges =
        appl_path_has_system_privileges(name);
    syscall_process_transition();
    sysret_to_user(entry, 0x202, user_stack_top, user_cr3);
    return 1;
}

static int user_exec_transition(const char* path, char* const argv[],
                               int kernel_transition) {
    int current_privileges = active_app_has_system_privileges;
    int argc;
    uint64_t next_cr3;
    tty_bind_execution_console();
    if (!vmm_nx_enabled()) {
        serial_puts("[sys_exec] refusing userspace without NX support\n");
        return -1;
    }
    serial_puts("[sys_exec] Loading: ");
    serial_puts(path);
    serial_puts("\n");

    if (!user_copy_argv(argv, &argc)) {
        serial_puts("[sys_exec] Invalid or oversized argument vector\n");
        return -1;
    }

    uint64_t entry = 0;
    framebuffer_set_pointer_busy(1);
    int loaded = load_elf(path, &entry);
    framebuffer_set_pointer_busy(0);
    if (!loaded) {
        serial_puts("[sys_exec] FAILED to load executable\n");
        return -1;
    }

    serial_puts("[sys_exec] New entry: 0x");
    {
        char buf[18];
        buf[0] = '0'; buf[1] = 'x';
        uint64_t v = entry;
        int i = 17; buf[i--] = '\0';
        do { buf[i--] = "0123456789abcdef"[v & 0xF]; v >>= 4; } while (v);
        serial_puts(&buf[i+1]);
    }
    serial_puts("\n");

    if (!refresh_active_user_image(
            argc, (const char (*)[USER_ARG_LENGTH])user_exec_args,
            &next_cr3)) {
        release_loaded_elf();
        serial_puts("[sys_exec] Failed to refresh active user address space\n");
        return -1;
    }
    release_loaded_elf();

    active_app_has_system_privileges = kernel_transition
        ? appl_path_has_system_privileges(path)
        : appl_exec_retains_system_privileges(current_privileges, path);
    syscall_process_transition();
    pending_user_rip = entry;
    pending_user_rsp = user_stack_top;
    pending_user_cr3 = next_cr3;
    serial_puts("[sys_exec] Staged address space; direct user transition\n");
    serial_puts("[sys_exec] pending RIP=0x");
    serial_hex_qword(pending_user_rip);
    serial_puts(" RSP=0x");
    serial_hex_qword(pending_user_rsp);
    serial_puts("\n");
    return (int)0xDEADBEEF;
}

int user_exec(const char* path, char* const argv[])
{
    return user_exec_transition(path, argv, 1);
}

int user_exec_from_user(const char* path, char* const argv[])
{
    return user_exec_transition(path, argv, 0);
}

int appl_user_child_depth(void)
{
    return user_child_depth;
}

int appl_run_user_child(const char* path, char* const argv[])
{
    user_parent_state_t* parent;
    vmm_address_space_t child_space = { 0 };
    uint64_t entry = 0;
    uint64_t child_cr3;
    int argc;

    if (!path || !active_user_space.pml4 ||
        user_child_depth >= USER_CHILD_MAX ||
        !user_copy_argv(argv, &argc))
        return -1;

    parent = &user_parent_states[user_child_depth];
    parent->address_space = active_user_space;
    parent->stack_top = user_stack_top;
    parent->has_system_privileges = active_app_has_system_privileges;
    memcpy(parent->heap_used, user_heap_used, sizeof(user_heap_used));
    memcpy(parent->heap_mapped, user_heap_mapped, sizeof(user_heap_mapped));
    memcpy(parent->heap_allocations, user_heap_allocations,
           sizeof(user_heap_allocations));

    if (!load_elf(path, &entry) ||
        !install_user_address_space(0, argc,
            (const char (*)[USER_ARG_LENGTH])user_exec_args, &child_space)) {
        release_loaded_elf();
        user_stack_top = parent->stack_top;
        return -1;
    }
    release_loaded_elf();

    active_user_space = child_space;
    active_app_has_system_privileges =
        appl_exec_retains_system_privileges(
            parent->has_system_privileges, path);
    memset(user_heap_used, 0, sizeof(user_heap_used));
    memset(user_heap_mapped, 0, sizeof(user_heap_mapped));
    memset(user_heap_allocations, 0, sizeof(user_heap_allocations));
    user_child_depth++;

    child_cr3 = (uint64_t)vmm_get_phys_addr(child_space.pml4);
    if (!child_cr3) {
        (void)appl_restore_user_parent();
        return -1;
    }
    pending_user_rip = entry;
    pending_user_rsp = user_stack_top;
    pending_user_cr3 = child_cr3;
    return (int)0xDEADBEEF;
}

int appl_fork_user_state(appl_user_fork_state_t* state)
{
    if (!state || state->address_space.pml4 || !active_user_space.pml4)
        return 0;
    memset(state, 0, sizeof(*state));
    if (!vmm_address_space_clone(&active_user_space,
                                 &state->address_space))
        return 0;
    state->stack_top = user_stack_top;
    state->has_system_privileges = active_app_has_system_privileges;
    memcpy(state->heap_used, user_heap_used, sizeof(user_heap_used));
    memcpy(state->heap_mapped, user_heap_mapped, sizeof(user_heap_mapped));
    memcpy(state->heap_allocations, user_heap_allocations,
           sizeof(user_heap_allocations));
    return 1;
}

int appl_run_forked_user_child(appl_user_fork_state_t* state)
{
    user_parent_state_t* parent;
    if (!state || !state->address_space.pml4 || !active_user_space.pml4 ||
        user_child_depth >= USER_CHILD_MAX)
        return 0;

    parent = &user_parent_states[user_child_depth];
    parent->address_space = active_user_space;
    parent->stack_top = user_stack_top;
    parent->has_system_privileges = active_app_has_system_privileges;
    memcpy(parent->heap_used, user_heap_used, sizeof(user_heap_used));
    memcpy(parent->heap_mapped, user_heap_mapped, sizeof(user_heap_mapped));
    memcpy(parent->heap_allocations, user_heap_allocations,
           sizeof(user_heap_allocations));

    active_user_space = state->address_space;
    memset(&state->address_space, 0, sizeof(state->address_space));
    user_stack_top = state->stack_top;
    active_app_has_system_privileges = state->has_system_privileges;
    memcpy(user_heap_used, state->heap_used, sizeof(user_heap_used));
    memcpy(user_heap_mapped, state->heap_mapped, sizeof(user_heap_mapped));
    memcpy(user_heap_allocations, state->heap_allocations,
           sizeof(user_heap_allocations));
    memset(state, 0, sizeof(*state));
    user_child_depth++;
    vmm_address_space_activate(&active_user_space);
    return 1;
}

int appl_restore_user_parent(void)
{
    user_parent_state_t* parent;
    uint64_t kernel_cr3 = (uint64_t)pml4_table;
    if (!user_child_depth)
        return 0;

    parent = &user_parent_states[user_child_depth - 1];
    __asm__ volatile("mov %0, %%cr3" :: "r"(kernel_cr3) : "memory");
    vmm_address_space_destroy(&active_user_space);
    active_user_space = parent->address_space;
    user_stack_top = parent->stack_top;
    active_app_has_system_privileges = parent->has_system_privileges;
    memcpy(user_heap_used, parent->heap_used, sizeof(user_heap_used));
    memcpy(user_heap_mapped, parent->heap_mapped, sizeof(user_heap_mapped));
    memcpy(user_heap_allocations, parent->heap_allocations,
           sizeof(user_heap_allocations));
    memset(parent, 0, sizeof(*parent));
    user_child_depth--;
    vmm_address_space_activate(&active_user_space);
    return 1;
}

int appl_run_file_argv(const char* name) {
    vga_print("[apl] About to load file: ", 0x0A);
    vga_print(name, 0x0A);
    vga_print("\n", 0x0A);
    uint8_t* load = (uint8_t*)APP_LOAD_ADDR;
    uint32_t size = 0;

    if (!fs_load_file(name, load, 1024 * 1024, &size)) {
        vga_print("[apl] Impossible de charger ", 0x0C);
        vga_print(name, 0x0C);
        vga_print("\n", 0x0C);
        return 0;
    }

    vga_print("[apl] Fichier charge, taille=", 0x0A);
    vga_put_uint_auto(size, 0x0A);
    vga_print("\n", 0x0A);

    uint32_t entry_addr = 0;

    if (size >= sizeof(struct appl_header)) {
        struct appl_header* h = (struct appl_header*)load;
        if (h->magic == APPL_MAGIC) {
            if (h->entry_offset >= size) {
                vga_print("[apl] decalage hors fichier\n", 0x0C);
                return 0;
            }
            entry_addr = h->entry_offset;
            vga_print("[apl] Chargement APPL ", 0x0A);
            vga_print(name, 0x0A);
        } else {
            vga_print("[apl] Dans le else binaire brut\n", 0x0A);
            entry_addr = 0;
            vga_print("[apl] Chargement binaire brut ", 0x0A);
            vga_print(name, 0x0A);
        }
    } else {
        vga_print("[apl] Dans le else petite taille\n", 0x0A);
        entry_addr = 0;
        vga_print("[apl] Chargement binaire brut (taille petite) ", 0x0A);
        vga_print(name, 0x0A);
    }

    vga_print("\n", 0x0A);

    void (*entry)(void) = (void(*)(void))(APP_LOAD_ADDR + entry_addr);

    vga_print("[apl] A propos d'appeler l'entree a 0x", 0x0A);
    vga_put_uint_auto((uintptr_t)entry, 0x0A);
    vga_print("\n", 0x0A);

    if (strcmp(name, "/run") == 0) {
        vga_print("[apl] Lancement de /run en mode noyau\n", 0x0A);
        entry();
        vga_print("\n[apl] Retour du /run\n", 0x0A);
        return 1;
    }
    return 1;
}

int appl_kill(int pid) {
    (void)pid;
    return 0;
}