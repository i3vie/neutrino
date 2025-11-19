.code64
section .text
global isr_stub_table

%macro ISR_NOERR 1
isr_stub_%1:
    push 0
    push %1
    jmp isr_common_stub
%endmacro

%macro ISR_ERR 1
isr_stub_%1:
    push %1
    jmp isr_common_stub
%endmacro

%macro ISR_GEN 1
%if %1 = 8 || %1 = 10 || %1 = 11 || %1 = 12 || %1 = 13 || %1 = 14 || %1 = 30
    ISR_ERR %1
%else
    ISR_NOERR %1
%endif
%endmacro

%assign __vec 0
%rep 256
    ISR_GEN __vec
%assign __vec __vec+1
%endrep

isr_common_stub:
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

    mov rdi, rsp ; the stack coincidentally is shaped like a C struct so we can just throw that into C if desires
    extern isr_handler
    call isr_handler

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

section .rodata
isr_stub_table:
%assign i 0
%rep 256
    dq isr_stub_%+i
%assign i i+1
%endrep
