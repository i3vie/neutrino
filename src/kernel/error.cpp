#include "error.hpp"

#include "drivers/console/console.hpp"
#include "drivers/log/logging.hpp"

namespace {
constexpr uint32_t kErrorBackground = 0xFF941616;
constexpr uint32_t kErrorForeground = 0xFFFFFFFF;

void print_registers(const InterruptFrame* regs) {
    if (regs == nullptr) {
        if (kconsole != nullptr) {
            kconsole->printf("Register dump unavailable.\n");
        }
        return;
    }

    if (kconsole == nullptr) {
        return;
    }

    kconsole->printf(" Register dump:\n");
    kconsole->printf(" RAX=%016x     RBX=%016x     RCX=%016x\n",
                     static_cast<unsigned long long>(regs->rax),
                     static_cast<unsigned long long>(regs->rbx),
                     static_cast<unsigned long long>(regs->rcx));
    kconsole->printf(" RDX=%016x     RSI=%016x     RDI=%016x\n",
                     static_cast<unsigned long long>(regs->rdx),
                     static_cast<unsigned long long>(regs->rsi),
                     static_cast<unsigned long long>(regs->rdi));
    kconsole->printf(" R8 =%016x     R9 =%016x     R10=%016x\n",
                     static_cast<unsigned long long>(regs->r8),
                     static_cast<unsigned long long>(regs->r9),
                     static_cast<unsigned long long>(regs->r10));
    kconsole->printf(" R11=%016x     R12=%016x     R13=%016x\n",
                     static_cast<unsigned long long>(regs->r11),
                     static_cast<unsigned long long>(regs->r12),
                     static_cast<unsigned long long>(regs->r13));
    kconsole->printf(" R14=%016x     R15=%016x     RBP=%016x\n",
                     static_cast<unsigned long long>(regs->r14),
                     static_cast<unsigned long long>(regs->r15),
                     static_cast<unsigned long long>(regs->rbp));
    kconsole->printf(" RIP=%016x     RSP=%016x  RFLAGS=%016x\n",
                     static_cast<unsigned long long>(regs->rip),
                     static_cast<unsigned long long>(regs->rsp),
                     static_cast<unsigned long long>(regs->rflags));
    kconsole->printf(" CS=%016x      SS=%016x\n",
                     static_cast<unsigned int>(regs->cs),
                     static_cast<unsigned int>(regs->ss));
}
}  // namespace

namespace error_screen {

[[noreturn]] void display(const char* primary,
                         const char* secondary,
                         const InterruptFrame* regs) {
    const char* main_message = primary ? primary : "";
    const char* info_message = secondary ? secondary : "";

    if (kconsole != nullptr) {
        kconsole->set_color(kErrorForeground, kErrorBackground);
        kconsole->clear();
        kconsole->putc('\n');
        kconsole->printf(" An error has occurred: %s%s\n", main_message, info_message);
        kconsole->printf(" Neutrino has been halted to prevent damage to your system or data.\n");
        kconsole->printf(" If possible, please record the following information for debugging purposes.\n\n");
        kconsole->putc('\n');
        print_registers(regs);
        kconsole->putc('\n');
        kconsole->printf(" Please create a bug report at https://github.com/i3vie/neutrino.\n");
        kconsole->printf(" Include the information above and any steps to reproduce the issue.\n");
        kconsole->printf(" Thank you for helping to improve Neutrino!\n");
        kconsole->putc('\n');
        kconsole->printf(" System halted.\n");
    }

    while (true) {
        asm volatile("cli; hlt");
    }
}

}  // namespace error_screen

