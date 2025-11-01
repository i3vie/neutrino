#include "arch/x86_64/syscall_table.hpp"

#include "../../drivers/log/logging.hpp"
#include "../../kernel/descriptor.hpp"
#include "../../kernel/file_io.hpp"
#include "../../kernel/loader.hpp"
#include "../../kernel/process.hpp"
#include "../../kernel/scheduler.hpp"
#include "../../fs/vfs.hpp"
#include "../../lib/mem.hpp"
#include "../../kernel/string_util.hpp"

namespace syscall {

namespace {

constexpr uint64_t kAbiMajor = 0;
constexpr uint64_t kAbiMinor = 1;

constexpr size_t kMaxExecImageSize = 512 * 1024;
alignas(16) uint8_t g_exec_buffer[kMaxExecImageSize];

bool load_program_image(const char* path, loader::ProgramImage& out_image) {
    if (path == nullptr) {
        return false;
    }

    vfs::FileHandle handle{};
    if (!vfs::open_file(path, handle)) {
        return false;
    }

    if (handle.size == 0 || handle.size > kMaxExecImageSize) {
        vfs::close_file(handle);
        return false;
    }

    size_t total = static_cast<size_t>(handle.size);
    size_t offset = 0;
    while (offset < total) {
        size_t chunk = total - offset;
        size_t read = 0;
        if (!vfs::read_file(handle,
                            offset,
                            g_exec_buffer + offset,
                            chunk,
                            read)) {
            vfs::close_file(handle);
            return false;
        }
        if (read == 0) {
            break;
        }
        offset += read;
    }

    vfs::close_file(handle);

    if (offset != total) {
        return false;
    }

    out_image.data = g_exec_buffer;
    out_image.size = total;
    out_image.entry_offset = 0;
    return true;
}

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
        case SystemCall::ProcessExec: {
            process::Process* proc = process::current();
            if (proc == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            const char* path = reinterpret_cast<const char*>(frame.rdi);
            loader::ProgramImage image{};
            if (!load_program_image(path, image)) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }

            process::Process* child = process::allocate();
            if (child == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }

            child->parent = proc;
            child->waiting_on = nullptr;
            child->exit_code = 0;
            child->has_exited = false;

            if (!loader::load_into_process(image, *child)) {
                child->state = process::State::Unused;
                child->pid = 0;
                child->parent = nullptr;
                child->waiting_on = nullptr;
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }

            child->state = process::State::Ready;
            scheduler::enqueue(child);

            proc->waiting_on = child;
            proc->state = process::State::Blocked;
            frame.rax = 0;
            return Result::Reschedule;
        }
        case SystemCall::Child: {
            process::Process* proc = process::current();
            if (proc == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }

            const char* path = reinterpret_cast<const char*>(frame.rdi);
            const char* args = reinterpret_cast<const char*>(frame.rsi);
            uint64_t flags = frame.rdx;
            (void)flags;

            loader::ProgramImage image{};
            if (!load_program_image(path, image)) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }

            process::Process* child = process::allocate();
            if (child == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }

            child->parent = proc;
            child->waiting_on = nullptr;
            child->exit_code = 0;
            child->has_exited = false;

            if (!loader::load_into_process(image, *child)) {
                child->state = process::State::Unused;
                child->pid = 0;
                child->parent = nullptr;
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }

            uint64_t arg_ptr = 0;
            if (args != nullptr && args[0] != '\0' &&
                child->stack_region.top > child->stack_region.base) {
                size_t arg_len = string_util::length(args);
                size_t total = arg_len + 1;
                uint64_t available = child->stack_region.top - child->stack_region.base;
                if (total < available) {
                    uint64_t dest = child->stack_region.top - static_cast<uint64_t>(total);
                    memcpy(reinterpret_cast<void*>(dest), args, total);
                    arg_ptr = dest;
                }
            }

            memset(&child->context, 0, sizeof(child->context));
            child->context.user_rip = child->user_ip;
            child->context.user_rsp = child->user_sp;
            child->context.user_rflags = 0x202;
            child->context.r11 = 0x202;
            child->context.rdi = arg_ptr;
            child->context.rsi = flags;
            child->context.rax = 0;
            child->has_context = true;

            child->state = process::State::Ready;
            scheduler::enqueue(child);

            frame.rax = static_cast<uint64_t>(child->pid);
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
        default: {
            log_message(LogLevel::Warn, "Unhandled syscall %llx", frame.rax);
            frame.rax = static_cast<uint64_t>(-1);
            return Result::Continue;
        }
    }
}

}  // namespace syscall
