#include <stddef.h>
#include <stdint.h>

#include "../../drivers/input/keyboard.hpp"
#include "../../drivers/interrupts/pic.hpp"
#include "../../drivers/log/logging.hpp"
#include "../../kernel/error.hpp"
#include "../../kernel/process.hpp"
#include "../../kernel/scheduler.hpp"
#include "percpu.hpp"
#include "lapic.hpp"
#include "isr.hpp"

// Interrupt number to name mapping
constexpr const char* exception_names[32] = {
    "DIVIDE_BY_ZERO",
    "DEBUG",
    "NMI",
    "BREAKPOINT",
    "OVERFLOW",
    "BOUND_RANGE_EXCEEDED",
    "INVALID_OPCODE",
    "DEVICE_NOT_AVAILABLE",
    "DOUBLE_FAULT",
    "COPROCESSOR_SEGMENT_OVERRUN",
    "INVALID_TSS",
    "SEGMENT_NOT_PRESENT",
    "STACK_SEGMENT_FAULT",
    "GENERAL_PROTECTION_FAULT",
    "PAGE_FAULT",
    "RESERVED",
    "x87_FLOATING_POINT_EXCEPTION",
    "ALIGNMENT_CHECK",
    "MACHINE_CHECK",
    "SIMD_FLOATING_POINT_EXCEPTION",
    "VIRTUALIZATION_EXCEPTION",
    "CONTROL_PROTECTION_EXCEPTION",
    "RESERVED",
    "RESERVED",
    "RESERVED",
    "RESERVED",
    "RESERVED",
    "RESERVED",
    "RESERVED",
    "RESERVED",
    "RESERVED",
    "RESERVED"
};

extern "C" void isr_handler(InterruptFrame* regs) {
    if (regs == nullptr) {
        return;
    }

    if (regs->int_no >= 32) {
        uint64_t irq = regs->int_no - 32;
        if (irq == 0) {
            scheduler::tick(*regs);
            pic::send_eoi(0);
            lapic::eoi();
            return;
        } else if (irq == 1) {
            keyboard::handle_irq();
            pic::send_eoi(1);
            lapic::eoi();
            return;
        } else if (regs->int_no == 0x40) {
            scheduler::tick(*regs);
            lapic::eoi();
            return;
        }
        lapic::eoi();
        return;
    }

    log_message(LogLevel::Error, "Exception %x %s",
                static_cast<unsigned int>(regs->int_no),
                regs->int_no < 32 ? exception_names[regs->int_no] : "Unknown");
    if (auto* cpu = percpu::current_cpu()) {
        log_message(LogLevel::Error,
                    "CPU: lapic=%u processor=%u",
                    cpu->lapic_id,
                    cpu->processor_id);
    }
    uint16_t selector = static_cast<uint16_t>(regs->err_code & 0xFFF8);
    bool external = (regs->err_code & 0x1) != 0;
    bool idt = (regs->err_code & 0x2) != 0;
    bool ldt = (regs->err_code & 0x4) != 0;
    log_message(LogLevel::Error, "Error code: %x (sel=%04x ext=%d idt=%d ldt=%d)",
                static_cast<unsigned int>(regs->err_code),
                static_cast<unsigned int>(selector),
                external ? 1 : 0,
                idt ? 1 : 0,
                ldt ? 1 : 0);
    uint64_t cr3_reg = 0;
    asm volatile("mov %%cr3, %0" : "=r"(cr3_reg));
    if (auto* cur = process::current()) {
        log_message(LogLevel::Error,
                    "Faulting process pid=%u cr3=%016llx",
                    static_cast<unsigned int>(cur->pid),
                    static_cast<unsigned long long>(cr3_reg));
    } else {
        log_message(LogLevel::Error,
                    "Faulting process unknown (cr3=%016llx)",
                    static_cast<unsigned long long>(cr3_reg));
    }
    if (regs->int_no == 14) {
        uint64_t cr2;
        asm volatile("mov %%cr2, %0" : "=r"(cr2));
        log_message(LogLevel::Error, "CR2=%016llx",
                    static_cast<unsigned long long>(cr2));
    }

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

    uint64_t* stack = reinterpret_cast<uint64_t*>(regs->rsp);
    if (stack != nullptr) {
        log_message(LogLevel::Debug,
                    "Stack[0..5]: %016llx %016llx %016llx %016llx %016llx %016llx",
                    static_cast<unsigned long long>(stack[0]),
                    static_cast<unsigned long long>(stack[1]),
                    static_cast<unsigned long long>(stack[2]),
                    static_cast<unsigned long long>(stack[3]),
                    static_cast<unsigned long long>(stack[4]),
                    static_cast<unsigned long long>(stack[5]));
    }

    const char* secondary =
        regs->int_no < 32 ? exception_names[regs->int_no] : "UNKNOWN_EXCEPTION";
    error_screen::display("UNHANDLED_CPU_EXCEPTION_", secondary, regs);
}
