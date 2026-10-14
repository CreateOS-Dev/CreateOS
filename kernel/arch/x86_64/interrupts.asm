; ============================================================
; CreateOS 0.0.1
; ============================================================
bits 64

section .text

extern interrupt_dispatch
extern pending_user_rip
extern pending_user_rsp
extern pending_user_cr3

%macro ISR_NO_ERROR 1
isr_%1:
    push qword 0
    push qword %1
    jmp interrupt_common
%endmacro

%macro ISR_ERROR 1
isr_%1:
    push qword %1
    jmp interrupt_common
%endmacro

ISR_NO_ERROR 0
ISR_NO_ERROR 1
ISR_NO_ERROR 2
ISR_NO_ERROR 3
ISR_NO_ERROR 4
ISR_NO_ERROR 5
ISR_NO_ERROR 6
ISR_NO_ERROR 7
ISR_ERROR    8
ISR_NO_ERROR 9
ISR_ERROR    10
ISR_ERROR    11
ISR_ERROR    12
ISR_ERROR    13
ISR_ERROR    14
ISR_NO_ERROR 15
ISR_NO_ERROR 16
ISR_ERROR    17
ISR_NO_ERROR 18
ISR_NO_ERROR 19
ISR_ERROR    20
ISR_ERROR    21
ISR_NO_ERROR 22
ISR_NO_ERROR 23
ISR_NO_ERROR 24
ISR_NO_ERROR 25
ISR_NO_ERROR 26
ISR_NO_ERROR 27
ISR_NO_ERROR 28
ISR_ERROR    29
ISR_ERROR    30
ISR_NO_ERROR 31

%assign irq 32
%rep 16
ISR_NO_ERROR irq
%assign irq irq + 1
%endrep

interrupt_common:
    cli
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov rdi, [rsp + 120]
    mov rsi, [rsp + 128]
    mov rdx, [rsp + 136]
    mov rcx, [rsp + 144]
    mov r8,  [rsp + 152]

    call interrupt_dispatch
    cmp eax, 0xDEADBEEF
    je user_exception_recovery

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    add rsp, 16
    iretq

user_exception_recovery:
    mov rax, [pending_user_cr3]
    mov qword [pending_user_cr3], 0
    mov cr3, rax

    mov rcx, [pending_user_rip]
    mov r11, 0x202
    mov r10, [pending_user_rsp]
    mov qword [pending_user_rip], 0
    mov qword [pending_user_rsp], 0

    mov rdx, r10
    sub r10, 40
    mov [r10], rcx
    mov qword [r10 + 8], 0x23
    mov [r10 + 16], r11
    mov [r10 + 24], rdx
    mov qword [r10 + 32], 0x1B
    mov rsp, r10
    iretq

section .rodata
global interrupt_stub_table
interrupt_stub_table:
%assign vector 0
%rep 48
    dq isr_%+vector
%assign vector vector + 1
%endrep

section .text
global idt_load
idt_load:
    lidt [rdi]
    ret