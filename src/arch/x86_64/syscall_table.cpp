#include "arch/x86_64/syscall_table.hpp"

#include "../../drivers/log/logging.hpp"

namespace syscall {

namespace {

constexpr uint64_t kAbiMajor = 0;
constexpr uint64_t kAbiMinor = 1;

}  // namespace

Result handle_syscall(SyscallFrame& frame) {
    switch (static_cast<SystemCall>(frame.rax)) {
        case SystemCall::AbiMajor: {
            frame.rax = kAbiMajor;
            return Result::Continue;
        }
        case SystemCall::AbiMinor: {
            frame.rax = kAbiMinor;
            return Result::Continue;
        }
        case SystemCall::Yield: {
            return Result::Reschedule;
        }
        default: {
            log_message(LogLevel::Warn, "Unhandled syscall %llx", frame.rax);
            frame.rax = static_cast<uint64_t>(-1);
            return Result::Continue;
        }
    }
}

}  // namespace syscall

