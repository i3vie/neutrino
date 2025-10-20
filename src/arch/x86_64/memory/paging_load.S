bits 64
global load_pml4

load_pml4:
    mov     cr3, rdi
    ret
