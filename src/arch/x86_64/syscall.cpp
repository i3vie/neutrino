#include "arch/x86_64/syscall.hpp"

#include <stdint.h>

#include "../../kernel/scheduler.hpp"
#include "arch/x86_64/gdt.hpp"
#include "arch/x86_64/syscall_table.hpp"

extern "C" void syscall_entry();

namespace syscall {

namespace {

constexpr uint32_t MSR_EFER  = 0xC0000080;
constexpr uint32_t MSR_STAR  = 0xC0000081;
constexpr uint32_t MSR_LSTAR = 0xC0000082;
constexpr uint32_t MSR_FMASK = 0xC0000084;

constexpr uint64_t EFER_SCE = 1ull << 0;
constexpr uint64_t RFLAGS_IF = 1ull << 9;

static_assert(sizeof(SyscallFrame) == 18 * sizeof(uint64_t),
              "SyscallFrame layout mismatch");

inline uint64_t read_msr(uint32_t msr) {
    uint32_t low = 0;
    uint32_t high = 0;
    asm volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return (static_cast<uint64_t>(high) << 32) | low;
}

inline void write_msr(uint32_t msr, uint64_t value) {
    uint32_t low = static_cast<uint32_t>(value & 0xFFFFFFFFu);
    uint32_t high = static_cast<uint32_t>(value >> 32);
    asm volatile("wrmsr" : : "c"(msr), "a"(low), "d"(high));
}

}  // namespace

extern "C" void syscall_dispatch(SyscallFrame* frame) {
    if (frame == nullptr) {
        return;
    }

    if (handle_syscall(*frame) == Result::Reschedule) {
        scheduler::reschedule(*frame);
    }
}

void init() {
    uint64_t star_value =
        (static_cast<uint64_t>(USER_CS) << 48) |
        (static_cast<uint64_t>(KERNEL_CS) << 32);
    write_msr(MSR_STAR, star_value);

    write_msr(MSR_LSTAR, reinterpret_cast<uint64_t>(&syscall_entry));
    write_msr(MSR_FMASK, RFLAGS_IF);

    uint64_t efer = read_msr(MSR_EFER);
    if ((efer & EFER_SCE) == 0) {
        write_msr(MSR_EFER, efer | EFER_SCE);
    }
}

}  // namespace syscall
