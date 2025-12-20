%define FRAME_RAX           0
%define FRAME_RBX           8
%define FRAME_RCX          16
%define FRAME_RDX          24
%define FRAME_RSI          32
%define FRAME_RDI          40
%define FRAME_RBP          48
%define FRAME_R8           56
%define FRAME_R9           64
%define FRAME_R10          72
%define FRAME_R11          80
%define FRAME_R12          88
%define FRAME_R13          96
%define FRAME_R14         104
%define FRAME_R15         112
%define FRAME_USER_RSP    120
%define FRAME_USER_RIP    128
%define FRAME_USER_RFLAGS 136
%define FRAME_SIZE        144

%define USER_CS           0x1B
%define USER_DS           0x23

section .text
global syscall_entry
extern syscall_dispatch
extern syscall_kernel_stack

syscall_entry:
    sub rsp, FRAME_SIZE

    mov [rsp + FRAME_RAX], rax
    mov [rsp + FRAME_RBX], rbx
    mov [rsp + FRAME_RCX], rcx
    mov [rsp + FRAME_RDX], rdx
    mov [rsp + FRAME_RSI], rsi
    mov [rsp + FRAME_RDI], rdi
    mov [rsp + FRAME_RBP], rbp
    mov [rsp + FRAME_R8],  r8
    mov [rsp + FRAME_R9],  r9
    mov [rsp + FRAME_R10], r10
    mov [rsp + FRAME_R11], r11
    mov [rsp + FRAME_R12], r12
    mov [rsp + FRAME_R13], r13
    mov [rsp + FRAME_R14], r14
    mov [rsp + FRAME_R15], r15

    lea r10, [rsp + FRAME_SIZE]
    mov [rsp + FRAME_USER_RSP], r10
    mov [rsp + FRAME_USER_RIP], rcx
    mov [rsp + FRAME_USER_RFLAGS], r11

    mov r12, rsp
    call syscall_kernel_stack
    test rax, rax
    jz .panic
    mov rsp, rax
    sub rsp, FRAME_SIZE
    mov rsi, r12
    mov rdi, rsp
    mov rcx, FRAME_SIZE / 8
    rep movsq

    mov r15, rsp
    mov rdi, rsp
    call syscall_dispatch

    mov rax, [r15 + FRAME_USER_RSP]
    mov rdx, [r15 + FRAME_USER_RFLAGS]
    mov rcx, [r15 + FRAME_USER_RIP]

    lea rsp, [r15 + FRAME_SIZE]
    sub rsp, 40
    mov qword [rsp + 32], USER_DS
    mov qword [rsp + 24], rax
    mov qword [rsp + 16], rdx
    mov qword [rsp + 8], USER_CS
    mov qword [rsp + 0], rcx

    mov rax, [r15 + FRAME_RAX]
    mov rbx, [r15 + FRAME_RBX]
    mov rcx, [r15 + FRAME_RCX]
    mov rdx, [r15 + FRAME_RDX]
    mov rsi, [r15 + FRAME_RSI]
    mov rdi, [r15 + FRAME_RDI]
    mov rbp, [r15 + FRAME_RBP]
    mov r8,  [r15 + FRAME_R8]
    mov r9,  [r15 + FRAME_R9]
    mov r10, [r15 + FRAME_R10]
    mov r11, [r15 + FRAME_R11]
    mov r12, [r15 + FRAME_R12]
    mov r13, [r15 + FRAME_R13]
    mov r14, [r15 + FRAME_R14]
    mov r15, [r15 + FRAME_R15]

    iretq

.panic:
    cli
.halt_loop:
    hlt
    jmp .halt_loop
