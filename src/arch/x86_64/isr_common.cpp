#include <stddef.h>
#include <stdint.h>

#include "../../drivers/input/keyboard.hpp"
#include "../../drivers/interrupts/pic.hpp"
#include "../../drivers/log/logging.hpp"
#include "../../kernel/scheduler.hpp"
#include "isr.hpp"

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

extern "C" void isr_handler(InterruptFrame* regs) {
    if (regs == nullptr) {
        return;
    }

    if (regs->int_no >= 32) {
        uint64_t irq = regs->int_no - 32;
        if (irq == 0) {
            if ((regs->cs & 0x3) != 0) {
                scheduler::reschedule_from_interrupt(*regs);
            }
        } else if (irq == 1) {
            keyboard::handle_irq();
        }
        if (irq < 16) {
            pic::send_eoi(static_cast<uint8_t>(irq));
        }
        return;
    }

    log_message(LogLevel::Error, "Exception %x %s",
                static_cast<unsigned int>(regs->int_no),
                regs->int_no < 32 ? exception_names[regs->int_no] : "Unknown");
    log_message(LogLevel::Error, "Error code: %x",
                static_cast<unsigned int>(regs->err_code));

    log_message(LogLevel::Debug, "RAX=%016x     RBX=%016x     RCX=%016x",
                static_cast<unsigned long long>(regs->rax),
                static_cast<unsigned long long>(regs->rbx),
                static_cast<unsigned long long>(regs->rcx));
    log_message(LogLevel::Debug, "RDX=%016x     RSI=%016x     RDI=%016x",
                static_cast<unsigned long long>(regs->rdx),
                static_cast<unsigned long long>(regs->rsi),
                static_cast<unsigned long long>(regs->rdi));
    log_message(LogLevel::Debug, "R8 =%016x     R9 =%016x     R10=%016x",
                static_cast<unsigned long long>(regs->r8),
                static_cast<unsigned long long>(regs->r9),
                static_cast<unsigned long long>(regs->r10));
    log_message(LogLevel::Debug, "R11=%016x     R12=%016x     R13=%016x",
                static_cast<unsigned long long>(regs->r11),
                static_cast<unsigned long long>(regs->r12),
                static_cast<unsigned long long>(regs->r13));
    log_message(LogLevel::Debug, "R14=%016x     R15=%016x     RBP=%016x",
                static_cast<unsigned long long>(regs->r14),
                static_cast<unsigned long long>(regs->r15),
                static_cast<unsigned long long>(regs->rbp));
    log_message(LogLevel::Debug, "RIP=%016x     RSP=%016x  RFLAGS=%016x",
                static_cast<unsigned long long>(regs->rip),
                static_cast<unsigned long long>(regs->rsp),
                static_cast<unsigned long long>(regs->rflags));
    log_message(LogLevel::Debug, "CS=%04x    SS=%04x",
                static_cast<unsigned int>(regs->cs),
                static_cast<unsigned int>(regs->ss));

    for (;;) asm("hlt");
}
