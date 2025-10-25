#include "userspace.hpp"

#include "arch/x86_64/gdt.hpp"
#include "arch/x86_64/tss.hpp"
#include "process.hpp"

namespace {

[[noreturn]] void transfer_to_userspace(uint64_t entry, uint64_t user_stack) {
    constexpr uint64_t kUserFlags = 0x202;
    asm volatile(
        "pushq %[ds]\n"
        "pushq %[stack]\n"
        "pushq %[flags]\n"
        "pushq %[cs]\n"
        "pushq %[entry]\n"
        "iretq\n"
        :
        : [ds] "r"(static_cast<uint64_t>(USER_DS)),
          [stack] "r"(user_stack),
          [flags] "r"(kUserFlags),
          [cs] "r"(static_cast<uint64_t>(USER_CS)),
          [entry] "r"(entry)
        : "memory");

    __builtin_unreachable();
}

}  // namespace

namespace userspace {

[[noreturn]] void enter_process(process::Process& proc) {
    set_rsp0(proc.kernel_stack_top);
    uint64_t entry = proc.user_ip;
    uint64_t user_stack = proc.user_sp & ~0xFull;
    transfer_to_userspace(entry, user_stack);
}

}  // namespace userspace

