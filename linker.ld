OUTPUT_FORMAT("elf64-x86-64")
ENTRY(_start)

SECTIONS
{
    . = 0x100000;

    .multiboot2 : ALIGN(8)
    {
        KEEP(*(.multiboot2))
    }

    kernel_start = .;

    .text : ALIGN(4K)
    {
        *(.text*)
    }

    .rodata : ALIGN(4K)
    {
        *(.rodata*)
    }
    .cmds : ALIGN(4K) {
        __start_cmds = .;
        KEEP(*(.cmds))
        __stop_cmds = .;
    }

    .syscalls : ALIGN(16) {
        __start_syscalls = .;
        KEEP(*(.syscalls))
        __stop_syscalls = .;
    }

    .data : ALIGN(4K)
    {
        *(.data*)
    }

    .bss : ALIGN(4K)
    {
        *(COMMON)
        *(.bss*)
    }

    kernel_end = .;
}
