BITS 64
DEFAULT REL
GLOBAL _start
EXTERN main

section .text
_start:
    xor rbp, rbp
    call main
    mov rdi, rax            ; exit status in rdi
.exit_again:
    mov rax, 2              ; SystemCall::Exit
    syscall
    jmp .exit_again
    
