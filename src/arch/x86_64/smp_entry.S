bits 64

section .text

global smp_ap_switch_stack
extern smp_ap_run

; rdi = top of the AP bootstrap stack
; rsi = Limine SMP info
; rdx = percpu state
smp_ap_switch_stack:
    cli
    mov rsp, rdi
    and rsp, -16
    xor rbp, rbp

    mov rdi, rsi
    mov rsi, rdx
    call smp_ap_run

.halt:
    cli
    hlt
    jmp .halt
