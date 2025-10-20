#include <stdint.h>
#include <stddef.h>
#include "../../drivers/console/console.hpp"

struct isr_regs {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t int_no;
    uint64_t err_code;
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
};

// Interrupt number to name mapping
constexpr const char* exception_names[32] = {
    "Divide By Zero",
    "Debug",
    "Non-maskable Interrupt",
    "Breakpoint",
    "Overflow",
    "Bound Range Exceeded",
    "Invalid Opcode",
    "Device Not Available",
    "Double Fault",
    "Coprocessor Segment Overrun",
    "Invalid TSS",
    "Segment Not Present",
    "Stack-Segment Fault",
    "General Protection Fault",
    "Page Fault",
    "Reserved",
    "x87 Floating-Point Exception",
    "Alignment Check",
    "Machine Check",
    "SIMD Floating-Point Exception",
    "Virtualization Exception",
    "Control Protection Exception",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Hypervisor Injection Exception",
    "VMM Communication Exception",
    "Security Exception",
    "Reserved"
};

extern "C" void isr_handler(isr_regs* regs) {
    kconsole->set_color(0xFFFF0000, 0x00000000);
    kconsole->printf("\nException: %d ", regs->int_no);
    if (regs->int_no < 32) {
        kconsole->printf("(%s)", exception_names[regs->int_no]);
    }
    kconsole->putc('\n');
    kconsole->printf("E=%x\n", regs->err_code);
    kconsole->set_color(0xFFFFFFFF, 0x00000000);
    // Print all register values, 3 per line
    kconsole->printf("RAX=%016x  RBX=%016x  RCX=%016x\n\n", regs->rax, regs->rbx, regs->rcx);
    kconsole->printf("RDX=%016x  RSI=%016x  RDI=%016x\n\n", regs->rdx, regs->rsi, regs->rdi);
    kconsole->printf("R8 =%016x  R9 =%016x  R10=%016x\n\n", regs->r8, regs->r9, regs->r10);
    kconsole->printf("R11=%016x  R12=%016x  R13=%016x\n\n", regs->r11, regs->r12, regs->r13);
    kconsole->printf("R14=%016x  R15=%016x  RBP=%016x\n\n", regs->r14, regs->r15, regs->rbp);
    kconsole->printf("RIP=%016x  RSP=%016x  RFLAGS=%016x\n\n", regs->rip, regs->rsp, regs->rflags);
    kconsole->printf("CS =%016x  SS =%016x\n", regs->cs, regs->ss);
    // Halt the system
    for (;;) asm("hlt");
}