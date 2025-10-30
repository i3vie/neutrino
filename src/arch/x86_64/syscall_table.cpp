#include "arch/x86_64/syscall_table.hpp"

#include "../../drivers/log/logging.hpp"
#include "../../kernel/descriptor.hpp"
#include "../../kernel/file_io.hpp"
#include "../../kernel/process.hpp"

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
        case SystemCall::Exit: {
            frame.rax = frame.rdi % 0xFFFF;
            return Result::Unschedule;
        }
        case SystemCall::Yield: {
            return Result::Reschedule;
        }
        case SystemCall::DescriptorOpen: {
            process::Process* proc = process::current();
            if (proc == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            uint32_t type = static_cast<uint32_t>(frame.rdi) &
                            descriptor::kTypeMask;
            int32_t handle =
                descriptor::open(*proc,
                                 proc->descriptors,
                                 type,
                                 frame.rsi,
                                 frame.rdx,
                                 frame.r10);
            frame.rax = static_cast<uint64_t>(static_cast<int64_t>(handle));
            return Result::Continue;
        }
        case SystemCall::DescriptorQuery: {
            process::Process* proc = process::current();
            if (proc == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            bool ok = false;
            uint32_t info = descriptor::query(
                proc->descriptors,
                static_cast<uint32_t>(frame.rdi),
                &ok);
            frame.rax =
                ok ? static_cast<uint64_t>(info) : static_cast<uint64_t>(-1);
            return Result::Continue;
        }
        case SystemCall::DescriptorRead: {
            process::Process* proc = process::current();
            if (proc == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            int64_t result = descriptor::read(*proc,
                                              proc->descriptors,
                                              static_cast<uint32_t>(frame.rdi),
                                              frame.rsi,
                                              frame.rdx,
                                              frame.r10);
            frame.rax = static_cast<uint64_t>(static_cast<int64_t>(result));
            return Result::Continue;
        }
        case SystemCall::DescriptorWrite: {
            process::Process* proc = process::current();
            if (proc == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            int64_t result = descriptor::write(*proc,
                                               proc->descriptors,
                                               static_cast<uint32_t>(frame.rdi),
                                               frame.rsi,
                                               frame.rdx,
                                               frame.r10);
            frame.rax = static_cast<uint64_t>(static_cast<int64_t>(result));
            return Result::Continue;
        }
        case SystemCall::DescriptorClose: {
            process::Process* proc = process::current();
            if (proc == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            bool ok = descriptor::close(*proc,
                                        proc->descriptors,
                                        static_cast<uint32_t>(frame.rdi));
            frame.rax = ok ? 0 : static_cast<uint64_t>(-1);
            return Result::Continue;
        }
        case SystemCall::FileOpen: {
            process::Process* proc = process::current();
            if (proc == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            const char* path = reinterpret_cast<const char*>(frame.rdi);
            int32_t handle = file_io::open_file(*proc, path);
            frame.rax = static_cast<uint64_t>(static_cast<int64_t>(handle));
            return Result::Continue;
        }
        case SystemCall::FileClose: {
            process::Process* proc = process::current();
            if (proc == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            bool ok =
                file_io::close_file(*proc, static_cast<uint32_t>(frame.rdi));
            frame.rax = ok ? 0 : static_cast<uint64_t>(-1);
            return Result::Continue;
        }
        case SystemCall::FileRead: {
            process::Process* proc = process::current();
            if (proc == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            int64_t result =
                file_io::read_file(*proc,
                                   static_cast<uint32_t>(frame.rdi),
                                   frame.rsi,
                                   frame.rdx);
            frame.rax = static_cast<uint64_t>(static_cast<int64_t>(result));
            return Result::Continue;
        }
        case SystemCall::FileWrite: {
            process::Process* proc = process::current();
            if (proc == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            int64_t result =
                file_io::write_file(*proc,
                                    static_cast<uint32_t>(frame.rdi),
                                    frame.rsi,
                                    frame.rdx);
            frame.rax = static_cast<uint64_t>(static_cast<int64_t>(result));
            return Result::Continue;
        }
        case SystemCall::FileCreate: {
            process::Process* proc = process::current();
            if (proc == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            const char* path = reinterpret_cast<const char*>(frame.rdi);
            int32_t handle = file_io::create_file(*proc, path);
            frame.rax = static_cast<uint64_t>(static_cast<int64_t>(handle));
            return Result::Continue;
        }
        case SystemCall::DirectoryOpen: {
            process::Process* proc = process::current();
            if (proc == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            const char* path = reinterpret_cast<const char*>(frame.rdi);
            int32_t handle = file_io::open_directory(*proc, path);
            frame.rax = static_cast<uint64_t>(static_cast<int64_t>(handle));
            return Result::Continue;
        }
        case SystemCall::DirectoryRead: {
            process::Process* proc = process::current();
            if (proc == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            int64_t result =
                file_io::read_directory(*proc,
                                        static_cast<uint32_t>(frame.rdi),
                                        frame.rsi);
            frame.rax = static_cast<uint64_t>(static_cast<int64_t>(result));
            return Result::Continue;
        }
        case SystemCall::DirectoryClose: {
            process::Process* proc = process::current();
            if (proc == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            bool ok = file_io::close_directory(*proc,
                                               static_cast<uint32_t>(frame.rdi));
            frame.rax = ok ? 0 : static_cast<uint64_t>(-1);
            return Result::Continue;
        }
        case SystemCall::Child: {
            process::Process* proc = process::current();
            if (proc == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            process::Process* child = process::allocate();
            if (child == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            child->parent = proc;
            child->code_region = {};
            child->stack_region = {};
            child->user_ip = 0;
            child->user_sp = 0;
            child->has_context = false;
            frame.rax = static_cast<uint64_t>(child->pid);
            return Result::Continue;
        }
        default: {
            log_message(LogLevel::Warn, "Unhandled syscall %llx", frame.rax);
            frame.rax = static_cast<uint64_t>(-1);
            return Result::Continue;
        }
    }
}

}  // namespace syscall
