; ============================================================
; CreateOS 0.0.1
; ============================================================
section .text

bits 64

extern syscall_dispatch
extern current_rsp_saved
extern syscall_stack_top
extern stack_top
extern pending_user_rip   ; from appl.c - for exec return
extern pending_user_rsp   ; from appl.c - for exec return
extern pending_user_cr3   ; from appl.c - for exec return
extern pending_process_frame
extern pending_process_user_rsp
extern pending_process_return_value

global syscall_entry

syscall_entry:
    swapgs
    mov [gs:current_rsp_saved], rsp
    mov rsp, [gs:syscall_stack_top]
    push r9                     ; save a6 (RSP + 112)
    push r8                     ; save a5 (RSP + 104)
    push r10                    ; save a4 (RSP + 96)
    push rdx                    ; save a3 (RSP + 88)
    push rsi                    ; save a2 (RSP + 80)
    push rdi                    ; save a1 (RSP + 72)
    push rax                    ; save num (RSP + 64)
    push rcx                    ; save user RIP
    push r11                    ; save user RFLAGS
    push rbx                    ; callee-saved
    push rbp                    ; callee-saved
    push r12                    ; callee-saved
    push r13                    ; callee-saved
    push r14                    ; callee-saved
    push r15                    ; callee-saved

    lea rax, [rsp]
    mov [syscall_frame_pointer], rax
    mov r10, [rsp + 112]        ; a6
    push 0                      ; Padding
    push 0                      ; Padding
    push r10                    ; 7th argument on stack (a6)

    mov edi, [rsp + 88]         ; num (RAX)
    mov rsi, [rsp + 96]         ; a1  (RDI)
    mov rdx, [rsp + 104]        ; a2  (RSI)
    mov rcx, [rsp + 112]        ; a3  (RDX)
    mov r8,  [rsp + 120]        ; a4  (R10)
    mov r9,  [rsp + 128]        ; a5  (R8)

    call syscall_dispatch
    add rsp, 24
    cmp eax, 0xC0DEC0DE
    je .process_return

    cmp eax, 0xDEADBEEF
    je .exec_return

    mov r10, rax

.normal_return:
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    pop r11                     ; user RFLAGS
    pop rcx                     ; user RIP
    pop rax                     ; syscall number (discard)
    add rsp, 48
    mov rax, r10

    mov rdx, [gs:current_rsp_saved]
    push qword 0x1B      ; user SS
    push rdx             ; user RSP
    push r11             ; user RFLAGS
    push qword 0x23      ; user CS
    push rcx             ; user RIP
    swapgs
    iretq

.process_return:
    mov r10, [pending_process_return_value]
    mov rsp, [pending_process_frame]
    mov rdx, [pending_process_user_rsp]
    mov [current_rsp_saved], rdx
    jmp .normal_return

.exec_return:
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    pop r11                     ; discard old RFLAGS
    pop rcx                     ; discard old RIP
    pop rax                     ; discard syscall number
    add rsp, 48                 ; discard old a1..a6

    ; Load new user context
    mov rcx, [pending_user_rip]   ; new user RIP
    mov r11, 0x202                ; User mode with IF
    mov r10, [pending_user_rsp]   ; new user RSP

    ; Clear general purpose registers
    xor rax, rax
    xor rdi, rdi
    xor rsi, rsi
    xor rdx, rdx
    xor rbx, rbx
    xor rbp, rbp
    xor r8,  r8
    xor r9,  r9
    xor r12, r12
    xor r13, r13
    xor r14, r14
    xor r15, r15

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
    swapgs
    iretq

global sysret_to_user

sysret_to_user:
    cli
    or rsi, 0x200
    mov cr3, rcx

    mov ax, 0x1B
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    mov r11, rdx
    sub r11, 40
    mov [r11], rdi
    mov qword [r11 + 8], 0x23
    mov [r11 + 16], rsi
    mov [r11 + 24], rdx
    mov qword [r11 + 32], 0x1B
    mov rsp, r11
    iretq

section .bss
align 16

global current_rsp_saved
global syscall_stack_top
global syscall_frame_pointer

current_rsp_saved:   resq 1
syscall_stack_top:   resq 1
syscall_frame_pointer: resq 1