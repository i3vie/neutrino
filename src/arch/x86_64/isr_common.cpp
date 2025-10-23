#include <stdint.h>
#include <stddef.h>
#include "../../drivers/log/logging.hpp"

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
    log_message(LogLevel::Error, "Exception %x %s", regs->int_no,
                regs->int_no < 32 ? exception_names[regs->int_no] : "Unknown");
    log_message(LogLevel::Error, "Error code: %x", regs->err_code);
    
    // Print all register values, 3 per line
    log_message(LogLevel::Debug, "RAX=%016x     RBX=%016x     RCX=%016x", regs->rax, regs->rbx, regs->rcx);
    log_message(LogLevel::Debug, "RDX=%016x     RSI=%016x     RDI=%016x", regs->rdx, regs->rsi, regs->rdi);
    log_message(LogLevel::Debug, "R8 =%016x     R9 =%016x     R10=%016x", regs->r8, regs->r9, regs->r10);
    log_message(LogLevel::Debug, "R11=%016x     R12=%016x     R13=%016x", regs->r11, regs->r12, regs->r13);
    log_message(LogLevel::Debug, "R14=%016x     R15=%016x     RBP=%016x", regs->r14, regs->r15, regs->rbp);
    log_message(LogLevel::Debug, "RIP=%016x     RSP=%016x  RFLAGS=%016x", regs->rip, regs->rsp, regs->rflags);
    log_message(LogLevel::Debug, "CS=%x    SS=%x", regs->cs, regs->ss);

    // Halt the system
    for (;;) asm("hlt");
}
