bits 64

%define S3_COPY_BASE 0x8000
%define S3_DATA_RSP (S3_COPY_BASE + s3_wake_data_rsp - s3_wake_template_start)
%define S3_DATA_RIP (S3_COPY_BASE + s3_wake_data_rip - s3_wake_template_start)
%define S3_DATA_CR3 (S3_COPY_BASE + s3_wake_data_cr3 - s3_wake_template_start)
%define S3_DATA_CR4 (S3_COPY_BASE + s3_wake_data_cr4 - s3_wake_template_start)
%define S3_DATA_GDTR (S3_COPY_BASE + s3_wake_data_gdtr - s3_wake_template_start)

global acpi_arch_s3_enter
global s3_wake_template_start
global s3_wake_template_end
global s3_wake_64_entry
global s3_wake_data_rsp
global s3_wake_data_rip
global s3_wake_data_cr3
global s3_wake_data_cr4

extern acpi_s3_resume_after_wake

section .text
acpi_arch_s3_enter:
    pushfq
    cli
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15

    mov [abs S3_DATA_RSP], rsp
    mov rax, .resume
    mov [abs S3_DATA_RIP], rax
    mov rax, cr3
    mov [abs S3_DATA_CR3], rax
    mov rax, cr4
    mov [abs S3_DATA_CR4], rax
    sgdt [abs S3_DATA_GDTR]

    mov r8d, edx
    mov r9d, esi
    mov r10d, ecx
    test r9w, r9w
    jz .sleep_a
    mov dx, r9w
    mov ax, r10w
    out dx, ax
.sleep_a:
    mov dx, di
    mov ax, r8w
    out dx, ax

.resume:
    call acpi_s3_resume_after_wake
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    popfq
    ret

section .rodata
align 16
s3_wake_template_start:
bits 16
s3_wake_legacy_entry:
    cli
    cld
    jmp 0x0000:(S3_COPY_BASE + s3_wake_real_mode - s3_wake_template_start)

s3_wake_real_mode:
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    o32 lgdt [S3_COPY_BASE + s3_wake_gdt_pointer - s3_wake_template_start]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp dword 0x18:(S3_COPY_BASE + s3_wake_protected - s3_wake_template_start)

bits 32
s3_wake_protected:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov eax, [S3_DATA_CR4]
    or eax, 1 << 5
    mov cr4, eax
    mov eax, [S3_DATA_CR3]
    mov cr3, eax
    mov ecx, 0xC0000080
    rdmsr
    or eax, 1 << 8
    wrmsr
    mov eax, cr0
    or eax, (1 << 31) | 1
    mov cr0, eax
    jmp 0x08:(S3_COPY_BASE + s3_wake_long_mode - s3_wake_template_start)

bits 64
s3_wake_64_entry:
s3_wake_long_mode:
    lgdt [abs S3_DATA_GDTR]
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov rax, [abs S3_DATA_CR4]
    mov cr4, rax
    mov eax, [abs S3_DATA_CR3]
    mov cr3, rax
    mov rsp, [abs S3_DATA_RSP]
    jmp qword [abs S3_DATA_RIP]

align 8
s3_wake_gdt:
    dq 0
    dq 0x00AF9A000000FFFF
    dq 0x00CF92000000FFFF
    dq 0x00CF9A000000FFFF
s3_wake_gdt_end:
s3_wake_gdt_pointer:
    dw s3_wake_gdt_end - s3_wake_gdt - 1
    dd S3_COPY_BASE + s3_wake_gdt - s3_wake_template_start
    dd 0
align 8
s3_wake_data_rsp:
    dq 0
s3_wake_data_rip:
    dq 0
s3_wake_data_cr3:
    dq 0
s3_wake_data_cr4:
    dq 0
s3_wake_data_gdtr:
    times 10 db 0
s3_wake_template_end:
