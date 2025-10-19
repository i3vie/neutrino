.code64
global load_gdt_ptr
section .text
load_gdt_ptr:
    lgdt [rdi]

    mov ax, 0x10            ; kernel data
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    lea rax, [rel tss]
    push rax
    push 0x08
    lretq
tss:
    mov ax, 0x28
    ltr ax

    ret
