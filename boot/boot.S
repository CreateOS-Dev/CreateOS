; ============================================================
; CreateOS 0.0.1
; ============================================================
bits 32

global _start
global pml4_table
global pdpt_table
global pd_table
global multiboot_info
global stack_top
global irq_stack_top

extern kernel_main
extern tss64

; --- Multiboot2 header : DOIT être en premier ---

section .multiboot2 align=8
header_start:
    dd 0xE85250D6          ; magic
    dd 0                   ; arch i386
    dd header_end - header_start
    dd 0x100000000 - (0xE85250D6 + 0 + (header_end - header_start))

    ; Request a linear 1024x768x32 framebuffer from GRUB.
    dw 5
    dw 0
    dd 20
    dd 1024
    dd 768
    dd 32
    align 8
    dw 0
    dw 0
    dd 8
header_end:

section .text align=16
_start:
    cli

    ; GRUB met le pointeur Multiboot2 dans EBX → on le sauvegarde
    mov [multiboot_info], ebx

    mov esp, stack_top

    ; Debug: write 'B' to port 0xE9 (QEMU/Bochs debugcon)
    mov al, 'B'
    out 0xE9, al

    ; ---------- Paging 4 niveaux, identity map 0–4 Gio en 2M ----------

    ; PML4[0] -> PDPT
    mov eax, pdpt_table
    or eax, 0b111
    mov [pml4_table], eax

    ; Four page directories, each covering 1 GiB with 2 MiB pages.
    mov eax, pd_table
    or eax, 0b111
    mov [pdpt_table], eax
    mov eax, pd_table1
    or eax, 0b111
    mov [pdpt_table + 8], eax
    mov eax, pd_table2
    or eax, 0b111
    mov [pdpt_table + 16], eax
    mov eax, pd_table3
    or eax, 0b111
    mov [pdpt_table + 24], eax

    xor ecx, ecx
.map_pd0:
    mov eax, 0x200000
    mul ecx
    or eax, 0b10001111
    mov [pd_table + ecx * 8], eax
    mov edx, eax
    add edx, 0x40000000
    mov [pd_table1 + ecx * 8], edx
    add edx, 0x40000000
    mov [pd_table2 + ecx * 8], edx
    add edx, 0x40000000
    mov [pd_table3 + ecx * 8], edx
    inc ecx
    cmp ecx, 512
    jne .map_pd0

    ; CR3
    mov eax, pml4_table
    mov cr3, eax

    ; PAE
    mov eax, cr4
    or eax, (1 << 5) | (1 << 9) | (1 << 10)
    mov cr4, eax

    ; Enable x87/SSE state before entering compiler-generated C code.
    mov eax, cr0
    and eax, 0xFFFFFFF3
    or eax, 1 << 1
    mov cr0, eax

    ; Long mode (EFER.LME)
    mov ecx, 0xC0000080
    rdmsr
    or eax, 1 << 8
    wrmsr

    ; Paging ON
    mov eax, cr0
    or eax, 1 << 31
    mov cr0, eax

    ; GDT 64-bit
    lgdt [gdt64.pointer]

    ; Far jump vers 64-bit
    jmp gdt64.code_segment:init_64

; ---------- 64-bit ----------

bits 64
init_64:
    mov ax, gdt64.data_segment
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    fninit

    ; Fill the TSS descriptor at GDT index 5 after relocation is available.
    mov rax, tss64
    mov word [gdt64 + 40], 103
    mov word [gdt64 + 42], ax
    shr rax, 16
    mov byte [gdt64 + 44], al
    shr rax, 8
    mov byte [gdt64 + 47], al
    shr rax, 8
    mov dword [gdt64 + 48], eax


    ; Debug: write 'C' to port 0xE9
    mov al, 'C'
    out 0xE9, al

    call kernel_main

.hang:
    hlt
    jmp .hang

; ---------- Données ----------

section .data align=4096
bits 64
pml4_table:
    times 512 dq 0
pdpt_table:
    times 512 dq 0
pd_table:
    times 512 dq 0
pd_table1:
    times 512 dq 0
pd_table2:
    times 512 dq 0
pd_table3:
    times 512 dq 0

gdt64:
    dq 0                                    ; index 0: NULL

.code_segment: equ $ - gdt64
    dq (1 << 43) | (1 << 44) | (1 << 47) | (1 << 53)  ; index 1: kernel CS

.data_segment: equ $ - gdt64
    dq 0x00CF92000000FFFF                              ; index 2: kernel DS

; User segments for SYSRET (Linux layout)
; STAR[63:48] = 0x13 -> SYSRET CS = 0x23 (index 4, RPL=3), SS = 0x1B (index 3, RPL=3)
.user_data_segment: equ $ - gdt64
    dq 0x00CFF2000000FFFF                              ; index 3: user DS, DPL=3

.user_code_segment: equ $ - gdt64
    dq 0x00AFFA000000FFFF                               ; index 4: user CS, DPL=3, L=1

; 64-bit available TSS descriptor, selector 0x28.
.tss_segment: equ $ - gdt64
    dw 0
    dw 0
    db 0
    db 0x89
    db 0
    db 0
    dd 0
    dd 0

dq 0                                    ; index 5: NULL
    dq 0                                    ; index 6: NULL

dq 0                                    ; index 5: NULL
    dq 0                                    ; index 6: NULL

.pointer:
    dw $ - gdt64 - 1
    dd gdt64

section .bss
align 8
multiboot_info:
    dq 0

align 16
stack_bottom:
    resb 65536
stack_top:

align 16
irq_stack_bottom:
    resb 16384
irq_stack_top:
