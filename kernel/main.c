// ============================================================
// CreateOS 0.0.1
// ============================================================


#include "drivers/vga.h"
#include "drivers/keyboard.h"
#include "drivers/mouse.h"
#include "drivers/ata.h"
#include "drivers/ahci.h"
#include "drivers/gpt.h"
#include "drivers/nvme.h"
#include "drivers/serial.h"
#include "drivers/timer.h"
#include "drivers/rtc.h"
#include "drivers/usb.h"
#include "drivers/audio.h"
#include "drivers/acpi.h"
#include "drivers/hpet.h"
#include "drivers/pci.h"
#include "drivers/e1000.h"
#include "drivers/rtl8139.h"
#include "drivers/virtio_net.h"
#include "drivers/netstack.h"
#include "drivers/framebuffer.h"
#include "drivers/tty.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/interrupts.h"
#include "arch/x86_64/tss.h"
#include "fs/fs.h"
#include "security/stack_protector.h"
#include "syscall.h"
#include "memory/pmm.h"
#include "memory/vmm.h"
#include "proc/proc.h"
#include "proc/appl.h"
#include "proc/user.h"
#include "lib/string.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

extern void syscall_entry(void);
extern uint64_t syscall_stack_top;
extern uint64_t stack_top;
extern uint64_t multiboot_info;
extern void sysret_to_user(uint64_t rip, uint64_t rflags, uint64_t rsp,
                          uint64_t cr3);

char kernel_cwd[256] = "/";
static int kernel_log_level = 2; /* 0 error, 1 warning, 2 info, 3 detail */

void kernel_setup(void);

static int boot_console_is_serial(void)
{
    uint8_t* info = (uint8_t*)(uintptr_t)multiboot_info;
    if (!info) return 0;
    uint32_t total = *(uint32_t*)info;
    uint8_t* tag = info + 8;
    while ((uintptr_t)(tag - info) < total) {
        uint32_t type = *(uint32_t*)tag;
        uint32_t size = *(uint32_t*)(tag + 4);
        if (type == 0) break;
        if (type == 1) {
            const char* cmdline = (const char*)(tag + 8);
            for (const char* p = cmdline; *p; p++) {
                if (!strncmp(p, "console=ttyS0", 13)) return 1;
            }
        }
        tag += (size + 7) & ~7U;
    }
    return 0;
}

static void log_level_init(void)
{
    uint8_t* info = (uint8_t*)(uintptr_t)multiboot_info;
    if (!info) return;
    uint32_t total = *(uint32_t*)info;
    uint8_t* tag = info + 8;
    while ((uintptr_t)(tag - info) < total) {
        uint32_t type = *(uint32_t*)tag;
        uint32_t size = *(uint32_t*)(tag + 4);
        if (type == 0) break;
        if (type == 1) {
            const char* cmdline = (const char*)(tag + 8);
            for (const char* p = cmdline; *p; p++) {
                if (!strncmp(p, "loglevel=quiet", 15)) kernel_log_level = 0;
                if (!strncmp(p, "loglevel=warn", 13)) kernel_log_level = 1;
                if (!strncmp(p, "loglevel=info", 13)) kernel_log_level = 2;
                if (!strncmp(p, "loglevel=debug", 14)) kernel_log_level = 3;
            }
        }
        tag += (size + 7) & ~7U;
    }
}

static void log_service(const char* service, const char* status) {
    if (kernel_log_level < 2) return;
    tty_write(service, 0x0F);
    tty_write(" : ", 0x0F);
    tty_write(status, 0x0F);
    tty_putchar('\n', 0x0F);
    serial_puts_raw(service); serial_puts_raw(" : "); serial_puts_raw(status); serial_puts_raw("\n");
}

static void log_service_detail(const char* service, const char* detail) {
    if (kernel_log_level < 3) return;
    tty_write("  ", 0x0F);
    tty_write(service, 0x0F);
    tty_write(" : ", 0x0F);
    tty_write(detail, 0x0F);
    tty_putchar('\n', 0x0F);
    serial_puts_raw("  "); serial_puts_raw(service); serial_puts_raw(" : "); serial_puts_raw(detail); serial_puts_raw("\n");
}

static void log_error(const char* service, const char* error) {
    tty_write("ERROR ", 0x0F);
    tty_write(service, 0x0F);
    tty_write(" : ", 0x0F);
    tty_write(error, 0x0F);
    tty_putchar('\n', 0x0F);
    serial_puts_raw("ERROR "); serial_puts_raw(service); serial_puts_raw(" : "); serial_puts_raw(error); serial_puts_raw("\n");
}

static void log_warning(const char* service, const char* warning) {
    if (kernel_log_level < 1) return;
    tty_write("WARN  ", 0x0F);
    tty_write(service, 0x0F);
    tty_write(" : ", 0x0F);
    tty_write(warning, 0x0F);
    tty_putchar('\n', 0x0F);
    serial_puts_raw("WARN "); serial_puts_raw(service); serial_puts_raw(" : "); serial_puts_raw(warning); serial_puts_raw("\n");
}

static void serial_put_uint(unsigned long value)
{
    char buffer[24];
    int index = 23;
    buffer[index] = 0;
    do {
        buffer[--index] = (char)('0' + (value % 10));
        value /= 10;
    } while (value && index > 0);
    serial_puts(&buffer[index]);
}

static void log_detail_uint(const char* service, const char* label,
                            unsigned long value)
{
    char buffer[24];
    int index = 23;
    buffer[index] = 0;
    do {
        buffer[--index] = (char)('0' + (value % 10));
        value /= 10;
    } while (value && index > 0);
    tty_write("  ", 0x0F);
    tty_write(service, 0x0F);
    tty_write(" : ", 0x0F);
    tty_write(label, 0x0F);
    tty_write(&buffer[index], 0x0F);
    tty_putchar('\n', 0x0F);
    serial_puts_raw("  ");
    serial_puts_raw(service);
    serial_puts_raw(" : ");
    serial_puts_raw(label);
    serial_puts_raw(&buffer[index]);
    serial_puts_raw("\n");
}

static void serial_put_hex(unsigned long value)
{
    char buffer[17];
    int index = 16;
    buffer[index] = 0;
    do {
        unsigned int digit = (unsigned int)(value & 0xF);
        buffer[--index] = (char)(digit < 10 ? '0' + digit : 'A' + digit - 10);
        value >>= 4;
    } while (value && index > 0);
    serial_puts(&buffer[index]);
}

static void halt_on_fatal(const char* service, const char* error) {
    log_error(service, error);
    vga_print("System halted - fatal error", 0x0C);
    vga_put_char_auto('\n', 0x0C);
    for (;;) __asm__ volatile("hlt");
}

// Wrapper functions to add error checking to void init functions
static bool init_keyboard(void) {
    log_service("Keyboard", "Initializing PS2 keyboard controller");
    if (!keyboard_init()) {
        log_warning("Keyboard", "PS/2 controller self-test failed; input fallback required");
        return false;
    }
    mouse_init();
    // Check if keyboard controller responded
    // For now assume success if no exception
    log_service_detail("Keyboard", "Interrupt handler registered on IRQ1");
    log_service_detail("Mouse", "PS/2 mouse enabled with wheel support");
    log_service("Keyboard", "OK");
    return true;
}

static bool init_timer(void) {
    log_service("Timer", "Initializing PIT timer at 1000Hz");
    timer_init();
    log_service_detail("Timer", "Interrupt handler registered on IRQ0");
    log_service("Timer", "OK");
    return true;
}

static bool init_rtc(void) {
    rtc_time_t now;
    log_service("RTC", "Initializing CMOS real-time clock");
    rtc_init();
    if (!rtc_read_time(&now)) {
        log_warning("RTC", "Unable to obtain a stable CMOS time");
        return false;
    }
    log_service_detail("RTC", "CMOS time source available");
    log_service("RTC", "OK");
    return true;
}

static void init_optional_devices(void) {
    log_service("ACPI", "Scanning firmware tables");
    if (acpi_init()) {
        log_service_detail("ACPI", acpi_revision() >= 2 ? "RSDP v2+ detected" : "RSDP v1 detected");
        if (acpi_madt_available())
            log_service_detail("ACPI", "MADT interrupt-controller table detected");
        if (acpi_ioapic_count() > 0)
            log_service_detail("ACPI", "IOAPIC descriptor detected");
        if (acpi_hpet_available()) {
            log_service("HPET", hpet_init(acpi_hpet_address()) ?
                        "64-bit monotonic clock initialized" :
                        "Initialization failed; PIT remains active");
        } else {
            log_service("HPET", "No usable ACPI table; PIT remains active");
        }
        log_service_detail("ACPI", acpi_s3_available() ?
                           "S3 suspend/resume path available" :
                           "S3 sleep state unavailable");
        log_service("ACPI", "Available");
    } else {
        log_service_detail("ACPI", "No valid RSDP found");
    }

    log_service_detail("APIC",
                       "IOAPIC routing deferred; using legacy PIC for timer reliability");

    log_service("PCI", "Scanning PCI configuration space");
    vga_print("  PCI devices: ", 0x0F);
    int pci_count = pci_device_count();
    vga_put_uint_auto((uint32_t)pci_count, 0x0F);
    vga_put_char_auto('\n', 0x0F);
    for (int i = 0; i < pci_count; i++) {
        const pci_device_t* device = pci_device_at(i);
        if (!device) continue;
        serial_puts("PCI : ");
        serial_put_hex(device->vendor);
        serial_puts(":");
        serial_put_hex(device->device);
        serial_puts(" class=");
        serial_put_hex(device->class_code);
        serial_puts(".");
        serial_put_hex(device->subclass);
        serial_puts(" prog=");
        serial_put_hex(device->prog_if);
        serial_puts("\n");
    }

    log_service("Network", "Probing supported Ethernet controllers");
    if (e1000_init()) {
        const uint8_t* mac = e1000_mac_address();
        log_service_detail("Network", "Intel 82540EM Ethernet controller initialized");
        if (mac) {
            serial_puts("Network : MAC=");
            for (int i = 0; i < 6; i++) {
                static const char digits[] = "0123456789ABCDEF";
                char text[3] = {
                    digits[mac[i] >> 4], digits[mac[i] & 0x0F], 0
                };
                if (i) serial_puts(":");
                serial_puts(text);
            }
            serial_puts("\n");
        }
    } else {
        log_service_detail("Network", "No supported Intel 82540EM controller");
    }
    if (rtl8139_init()) {
        log_service_detail("Network", "Realtek RTL8139 Ethernet controller initialized");
    }
    if (virtio_net_init()) {
        log_service_detail("Network", "VirtIO-net legacy PCI controller initialized");
    }
    net_stack_init();
    net_stack_poll();

    log_service("USB", "Scanning PCI USB controllers");
    if (usb_init()) {
        log_service_detail("USB", usb_controller_name());
        vga_print("  USB controllers: ", 0x0F);
        vga_put_uint_auto((uint32_t)usb_controller_count(), 0x0F);
        vga_put_char_auto('\n', 0x0F);
        log_service("USB", usb_controller_present()
                      ? "Controller initialized; USB device polling enabled"
                      : "No usable supported controller; continuing without USB");
    } else {
        log_service_detail("USB", "No supported controller detected");
    }

    log_service("Audio", "Scanning PCI audio controllers");
    if (audio_init()) {
        log_service_detail("Audio", audio_backend_name());
        log_service("Audio", "Audio controller detected");
    } else {
        log_service_detail("Audio", "No audio controller detected");
    }
    log_service("GPT", "Scanning partition table");
    if (gpt_init()) {
        log_service_detail("GPT", "GPT header and partition entries validated");
        log_service("GPT", "Available");
    } else {
        log_service_detail("GPT", "No valid GPT on the boot disk");
    }
    log_service("NVMe", "Scanning PCI storage controllers");
    if (nvme_init()) {
        log_service("NVMe", "Controller initialized");
        serial_puts("NVMe : model=");
        serial_puts(nvme_model());
        serial_puts(" namespaces=");
        serial_put_uint(nvme_namespace_count());
        serial_puts(" block_size=");
        serial_put_uint(nvme_namespace_block_size());
        serial_puts("\n");
    } else
        log_service_detail("NVMe", "No supported NVMe controller detected");
}

static bool init_serial(void) {
    if (!serial_init()) {
        log_warning("Serial", "COM1 UART unavailable; continuing without serial I/O");
        return false;
    }
    log_service("Serial", "Initializing serial port (COM1)");
    log_service_detail("Serial", "8250 UART configured 115200 8N1");
    log_service("Serial", "OK");
    return true;
}

static bool init_ata(void) {
    log_service("ATA", "Detecting storage devices");
    if (ahci_init()) {
        log_service_detail("ATA", "AHCI SATA controller initialized");
        log_service("ATA", "AHCI OK");
        return true;
    }
    if (!ata_detect()) {
        log_warning("ATA", "No ATA disk on primary controller (0x1F0)");
        log_service_detail("ATA", "Continuing - may boot from removable media");
        return true; // Not fatal
    }
    log_service("ATA", "ATA device detected on primary controller");
    if (ata_is_atapi()) {
        log_service_detail("ATA", "Device type: ATAPI CD-ROM");
    } else {
        log_service_detail("ATA", "Device type: ATA hard disk");
    }
    log_service("ATA", "OK");
    return true;
}

static bool init_pmm(void) {
    log_service("Memory", "Initializing physical memory manager");
    extern void pmm_init(uint32_t, uint32_t);
    extern void pmm_init_mmap(const void*, uint32_t, uint32_t);
    bool mmap_initialized = false;
    uint8_t* info = (uint8_t*)(uintptr_t)multiboot_info;
    if (info) {
        uint32_t total = *(uint32_t*)info;
        uint8_t* tag = info + 8;
        while ((uintptr_t)(tag - info) < total) {
            uint32_t type = *(uint32_t*)tag;
            uint32_t size = *(uint32_t*)(tag + 4);
            if (type == 0) break;
            if (type == 6 && size >= 24) {
                uint32_t entry_size = *(uint32_t*)(tag + 8);
                if (entry_size < 24) entry_size = 24;
                pmm_init_mmap(tag + 16, size - 16, entry_size);
                mmap_initialized = true;
            }
            tag += (size + 7) & ~7U;
        }
    }
    if (!mmap_initialized) {
        pmm_init(640, 1024*1024 - 1024);
    }
    pmm_reserve_range(0x000E0000, 0x00020000);
    pmm_reserve_range(0x000A0000, 0x00060000);
    if (framebuffer_info()->available) {
        const framebuffer_info_t* fb = framebuffer_info();
        pmm_reserve_range(fb->address, (uint64_t)fb->pitch * fb->height);
    }
    for (int i = 0; i < pci_device_count(); i++) {
        const pci_device_t* device = pci_device_at(i);
        if (!device) continue;
        for (int bar_index = 0; bar_index < 6; bar_index++) {
            const pci_bar_t* bar = pci_bar_info(device, bar_index);
            if (bar && bar->present &&
                (bar->type == 0 || bar->type == 2) &&
                bar->base < 0x40000000ULL)
                pmm_reserve_range(bar->base, bar->size);
        }
    }
    // Verify PMM initialized correctly
    if (pmm_get_free_frames() == 0) {
        log_error("Memory", "PMM reports zero free frames");
        return false;
    }
    log_service_detail("Memory", "PMM initialized with bitmap allocator");
    log_service("Memory", "OK");
    return true;
}

static bool init_vmm(void) {
    log_service("Memory", "Initializing virtual memory manager");
    extern void vmm_init(void);
    vmm_init();
    log_service_detail("Memory", "VMM initialized with 4-level paging");
    if (vmm_nx_enabled())
        log_service("Memory", "NX page protection enabled");
    else
        log_warning("Memory", "CPU NX page protection unavailable");
    log_service("Memory", "OK");
    return true;
}

static void syscall_msr_init(void) {
    log_service("MSR", "Initializing SYSCALL MSRs");

    uint64_t star = ((uint64_t)0x23 << 48) | ((uint64_t)0x08 << 32) | 0;
    __asm__ volatile("wrmsr" :: "a"(star & 0xFFFFFFFF), "d"(star >> 32), "c"(0xC0000081));
    log_service_detail("MSR", "STAR register configured (SYSRET CS=0x23, SS=0x1B)");

    uint64_t lstar = (uint64_t)syscall_entry;
    __asm__ volatile("wrmsr" :: "a"(lstar & 0xFFFFFFFF), "d"(lstar >> 32), "c"(0xC0000082));
    log_service_detail("MSR", "LSTAR register set to syscall entry point");

    uint64_t fmask = 0x4700;
    __asm__ volatile("wrmsr" :: "a"(fmask & 0xFFFFFFFF), "d"(fmask >> 32), "c"(0xC0000084));
    log_service_detail("MSR", "FMASK set to 0x4700");

    uint32_t low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(0xC0000080));
    uint64_t efer = ((uint64_t)high << 32) | low;
    efer |= 1;
    __asm__ volatile("wrmsr" :: "a"(efer & 0xFFFFFFFF), "d"(efer >> 32), "c"(0xC0000080));
    log_service_detail("MSR", "EFER.SCE enabled for SYSCALL/SYSRET");

    __asm__ volatile("wrmsr" :: "a"(0), "d"(0), "c"(0xC0000102));
    log_service_detail("MSR", "Kernel GS base cleared");

    syscall_stack_top = (uint64_t)&stack_top;
    log_service("MSR", "Syscall stack top initialized");
    log_service("MSR", "OK");
}

void syscall_resume_init(void)
{
    syscall_msr_init();
}

static void verify_kernel_integrity(void) {
    log_service("Integrity", "Verifying kernel integrity");

    if ((uintptr_t)syscall_entry == 0) {
        halt_on_fatal("Integrity", "syscall_entry is NULL");
    }
    log_service_detail("Integrity", "syscall_entry validated");

    if ((uintptr_t)sysret_to_user == 0) {
        halt_on_fatal("Integrity", "sysret_to_user is NULL");
    }
    log_service_detail("Integrity", "sysret_to_user validated");

    volatile uint16_t* vga = (volatile uint16_t*)0xB8000;
    for (int i = 0; i < 80*25; i++) {
        if (vga[i] != 0 && vga[i] != 0x0720) {
            break;
        }
    }
    log_service_detail("Integrity", "VGA text buffer accessible at 0xB8000");

    uint64_t rsp;
    __asm__ volatile("mov %%rsp, %0" : "=r"(rsp));
    if (rsp % 16 != 0) {
        log_warning("Integrity", "Stack not 16-byte aligned");
    } else {
        log_service_detail("Integrity", "Stack 16-byte aligned");
    }

    log_service("Integrity", "OK - all checks passed");
}

void kernel_main(void) {
    vga_clear();
    framebuffer_init((const void*)(uintptr_t)multiboot_info);
    log_level_init();

    // Initialize serial first for early logging
    (void)init_serial();
    if (!x86_64_cpu_init()) {
        halt_on_fatal("CPU", "Required x86-64 processor features are unavailable");
    }

    log_service("Boot", "CreateOS kernel starting");
    if (framebuffer_info()->available)
        log_service("Framebuffer", "Multiboot framebuffer available");
    else
        log_service_detail("Framebuffer", "Using VGA text fallback");
    log_service("Boot", "Early initialization complete");
    log_detail_uint("Boot", "Multiboot info bytes=", *(uint32_t*)(uintptr_t)multiboot_info);

    // Stack protector
    if (!__stack_chk_init())
        halt_on_fatal("Security", "Hardware entropy unavailable for stack protector");
    log_service("Security", "Stack protector initialized");

    // Hardware drivers with error handling
    log_service("Driver", "Initializing hardware drivers");
    tty_init();
    tty_select_serial(boot_console_is_serial());
    log_service_detail("Console", boot_console_is_serial() ?
                       "Selected serial console ttyS0" :
                       "Selected framebuffer console tty1");

    if (!init_keyboard())
        log_warning("Keyboard", "Continuing without PS/2 keyboard input");

    if (!init_timer()) {
        halt_on_fatal("Timer", "Failed to initialize PIT timer");
    }

    tss_init((uint64_t)(uintptr_t)&stack_top);
    tss_load();
    log_service("TSS", "Ring-0 interrupt stack loaded");

    if (!init_rtc()) {
        log_warning("RTC", "Continuing without real-time clock reads");
    }

    log_service("Interrupts", "Installing IDT and remapping PIC");
    interrupts_init();
    log_service("Interrupts", "IDT and hardware IRQs enabled");

    // Memory management with error handling
    if (!init_pmm()) {
        halt_on_fatal("Memory", "Physical memory manager initialization failed");
    }

    if (!init_vmm()) {
        halt_on_fatal("Memory", "Virtual memory manager initialization failed");
    }

#ifdef PROC_CONTEXT_SELFTEST
    extern void proc_init(void);
    proc_init();
#endif

    if (!init_ata()) {
        log_warning("Storage", "No ATA-compatible storage detected; continuing discovery");
    } else {
        log_service("Storage", "ATA/AHCI storage initialized");
    }

    init_optional_devices();
    if (framebuffer_info()->available && !framebuffer_map())
        log_warning("Framebuffer", "Unable to map framebuffer; keeping VGA output");
    if (framebuffer_ready()) {
        if (framebuffer_font_init())
            log_service("Fonts", "Noto Sans TrueType font initialized");
        else
            log_warning("Fonts", "Noto Sans unavailable; using bitmap fonts");
    }
    /* Raw diagnostics stay on serial; user TTY output owns the framebuffer. */
    serial_set_display_mirror(0);
    if (framebuffer_ready()) {
        const framebuffer_info_t* fb = framebuffer_info();
        log_service_detail("Framebuffer", "Linear framebuffer mapped");
        log_detail_uint("Framebuffer", "Width=", fb->width);
        log_detail_uint("Framebuffer", "Height=", fb->height);
        log_detail_uint("Framebuffer", "Pitch=", fb->pitch);
        log_detail_uint("Framebuffer", "Bits per pixel=", fb->bpp);
    }

    // Syscall MSR setup
    syscall_msr_init();

    // Kernel integrity verification
    verify_kernel_integrity();

    // Core system setup
    log_service("Setup", "Starting core system initialization");
    kernel_setup();

    // Syscall subsystem
    log_service("Syscall", "Initializing syscall dispatcher");
    syscall_init();
    log_service_detail("Syscall", "System call table built and dispatcher ready");

#ifdef ACPI_S3_SELFTEST
    if (!acpi_s3_available()) {
        serial_puts("[s3-test] FAIL: ACPI S3 not advertised\n");
        for (;;) __asm__ volatile("hlt");
    }
    serial_puts("[s3-test] ENTERING S3\n");
    if (!acpi_suspend_s3()) {
        serial_puts("[s3-test] FAIL: S3 entry rejected\n");
        for (;;) __asm__ volatile("hlt");
    }
    serial_puts("[s3-test] RESUMED FROM S3\n");
#endif

    // Process subsystem
    log_service("Process", "Initializing process manager");
    extern void proc_init(void);
    proc_init();
    log_service_detail("Process", "Scheduler and process table initialized");

    log_service("Boot", "Kernel initialization complete");
    log_service("Boot", "Starting /sbin/init as PID 1");
    if (!appl_launch_user("/sbin/init")) {
        log_error("Process", "Unable to load PID 1; keeping kernel alive");
        log_service("Boot", "Recovery mode unavailable");
    }

    for (;;) {
        __asm__ volatile("hlt");
    }
}

static int keyboard_layout_name_valid(const char* name, size_t length)
{
    if (!name || length < 5 || length >= 64 ||
        memcmp(name + length - 4, ".lyt", 4) != 0)
        return 0;
    for (size_t i = 0; i < length - 4; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_'))
            return 0;
    }
    return 1;
}

static void init_keyboard_layout(void)
{
    static const char default_path[] = "/etc/keyboards/us.lyt";
    static const char directory[] = "/etc/keyboards/";
    char name[64];
    char path[96];
    uint32_t size = 0;
    size_t length = 0;

    if (fs_load_file("/etc/keyboards/default", (uint8_t*)name,
                     sizeof(name) - 1, &size) && size < sizeof(name)) {
        name[size] = 0;
        while (size && (name[size - 1] == '\n' || name[size - 1] == '\r'))
            name[--size] = 0;
        length = size;
    }

    if (!keyboard_layout_name_valid(name, length)) {
        log_warning("Keyboard", "Invalid or missing layout selection; using US");
        strcpy(path, default_path);
    } else {
        size_t directory_length = sizeof(directory) - 1;
        memcpy(path, directory, directory_length);
        memcpy(path + directory_length, name, length + 1);
    }

    if (!keyboard_load_layout(path)) {
        log_warning("Keyboard", "Selected layout is invalid; retaining built-in US map");
        if (strcmp(path, default_path) != 0 &&
            !keyboard_load_layout(default_path))
            log_warning("Keyboard", "US layout file is invalid; retaining built-in map");
    } else {
        log_service("Keyboard", path);
    }
}

static int cursor_theme_name_valid(const char* name, size_t length)
{
    if (!name || length < 6 || length >= 64 ||
        memcmp(name + length - 6, ".srbmp", 6) != 0)
        return 0;
    for (size_t i = 0; i < length - 6; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.'))
            return 0;
    }
    return 1;
}

static void init_cursor_theme(void)
{
    char name[64] = "classic.srbmp";
    char path[112] = "/usr/share/createos/cursors/";
    char sprite[4096];
    uint32_t name_size = 0, sprite_size = 0;
    size_t length;
    if (fs_load_file("/etc/cursors/default", (uint8_t*)name,
                     sizeof(name) - 1, &name_size) &&
        name_size < sizeof(name)) {
        name[name_size] = 0;
        while (name_size &&
               (name[name_size - 1] == '\n' || name[name_size - 1] == '\r'))
            name[--name_size] = 0;
    } else {
        strcpy(name, "classic.srbmp");
    }

    length = strlen(name);
    if (!cursor_theme_name_valid(name, length)) {
        log_warning("Cursor", "Invalid theme selection; using classic cursor");
        strcpy(name, "classic.srbmp");
        length = strlen(name);
    }

    size_t prefix_length = strlen(path);
    memcpy(path + prefix_length, name, length + 1);
    if (!fs_load_file(path, (uint8_t*)sprite, sizeof(sprite), &sprite_size) ||
        !framebuffer_set_pointer_sprite(sprite, sprite_size)) {
        log_warning("Cursor", "Selected cursor theme is invalid; using built-in pointer");
        return;
    }
    log_service("Cursor", name);
}

void kernel_setup(void) {
    log_service("Setup", "Mounting root filesystem");

    if (!fs_init()) {
        halt_on_fatal("Setup", "Failed to mount root filesystem");
    }
    acpi_idle_configure();
    init_keyboard_layout();
    init_cursor_theme();
    kernel_users_init();
    log_service("Setup", "Root filesystem mounted successfully");
    log_service("Setup", "Core system setup complete");
}