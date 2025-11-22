#include "arch/x86_64/syscall_table.hpp"

#include "../../drivers/log/logging.hpp"
#include "../../kernel/descriptor.hpp"
#include "../../kernel/file_io.hpp"
#include "../../kernel/loader.hpp"
#include "../../kernel/process.hpp"
#include "../../kernel/path_util.hpp"
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

uint64_t place_args_on_stack(process::Process& child, const char* args) {
    if (args == nullptr) {
        return 0;
    }
    size_t len = string_util::length(args);
    if (len == 0) {
        return 0;
    }

    uint64_t base = child.stack_region.base;
    uint64_t top = child.stack_region.top;
    if (top <= base + 1) {
        return 0;
    }

    uint64_t available = top - base;
    if (available <= 1) {
        return 0;
    }

    if (len + 1 > static_cast<size_t>(available)) {
        len = static_cast<size_t>(available) - 1;
    }

    uint64_t dest = top - static_cast<uint64_t>(len + 1);
    char* dest_ptr = reinterpret_cast<char*>(dest);
    for (size_t i = 0; i < len; ++i) {
        dest_ptr[i] = args[i];
    }
    dest_ptr[len] = '\0';
    return dest;
}

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
            uint32_t type = static_cast<uint32_t>(frame.rdi & 0xFFFFu);
            uint32_t handle =
                descriptor::open(*proc,
                                 proc->descriptors,
                                 type,
                                 frame.rsi,
                                 frame.rdx,
                                 frame.r10);
            frame.rax = (handle == descriptor::kInvalidHandle)
                            ? static_cast<uint64_t>(-1)
                            : static_cast<uint64_t>(handle);
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
                                              static_cast<uint32_t>(frame.rdi &
                                                                     0xFFFFFFFFu),
                                              frame.rsi,
                                              frame.rdx,
                                              frame.r10);
            if (result == descriptor::kWouldBlock) {
                return Result::Reschedule;
            }
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
                                               static_cast<uint32_t>(frame.rdi &
                                                                      0xFFFFFFFFu),
                                               frame.rsi,
                                               frame.rdx,
                                               frame.r10);
            if (result == descriptor::kWouldBlock) {
                return Result::Reschedule;
            }
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
                                        static_cast<uint32_t>(frame.rdi &
                                                               0xFFFFFFFFu));
            frame.rax = ok ? 0 : static_cast<uint64_t>(-1);
            return Result::Continue;
        }
        case SystemCall::DescriptorGetType: {
            process::Process* proc = process::current();
            if (proc == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            uint16_t type = 0;
            bool ok = descriptor::get_type(
                proc->descriptors,
                static_cast<uint32_t>(frame.rdi & 0xFFFFFFFFu),
                type);
            frame.rax =
                ok ? static_cast<uint64_t>(type) : static_cast<uint64_t>(-1);
            return Result::Continue;
        }
        case SystemCall::DescriptorTestFlag: {
            process::Process* proc = process::current();
            if (proc == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            bool value = false;
            bool ok = descriptor::test_flag(
                proc->descriptors,
                static_cast<uint32_t>(frame.rdi & 0xFFFFFFFFu),
                static_cast<uint64_t>(frame.rsi),
                value);
            if (!ok) {
                frame.rax = static_cast<uint64_t>(-1);
            } else {
                frame.rax = value ? 1 : 0;
            }
            return Result::Continue;
        }
        case SystemCall::DescriptorGetFlags: {
            process::Process* proc = process::current();
            if (proc == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            uint64_t flags = 0;
            bool ok = descriptor::get_flags(
                proc->descriptors,
                static_cast<uint32_t>(frame.rdi & 0xFFFFFFFFu),
                frame.rsi != 0,
                flags);
            frame.rax =
                ok ? flags : static_cast<uint64_t>(-1);
            return Result::Continue;
        }
        case SystemCall::DescriptorGetProperty: {
            process::Process* proc = process::current();
            if (proc == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            int result = descriptor::get_property(
                *proc,
                proc->descriptors,
                static_cast<uint32_t>(frame.rdi & 0xFFFFFFFFu),
                static_cast<uint32_t>(frame.rsi & 0xFFFFFFFFu),
                frame.rdx,
                frame.r10);
            frame.rax = (result == 0) ? 0 : static_cast<uint64_t>(-1);
            return Result::Continue;
        }
        case SystemCall::DescriptorSetProperty: {
            process::Process* proc = process::current();
            if (proc == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            int result = descriptor::set_property(
                *proc,
                proc->descriptors,
                static_cast<uint32_t>(frame.rdi & 0xFFFFFFFFu),
                static_cast<uint32_t>(frame.rsi & 0xFFFFFFFFu),
                frame.rdx,
                frame.r10);
            frame.rax = (result == 0) ? 0 : static_cast<uint64_t>(-1);
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
            const char* path_user = reinterpret_cast<const char*>(frame.rdi);
            const char* args = reinterpret_cast<const char*>(frame.rsi);
            uint64_t flags = frame.rdx;
            const char* cwd_user = reinterpret_cast<const char*>(frame.r10);

            if (path_user == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }

            size_t path_len = string_util::length(path_user);
            if (path_len == 0 || path_len >= path_util::kMaxPathLength) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }

            char path_input[path_util::kMaxPathLength];
            char resolved_exec[path_util::kMaxPathLength];
            string_util::copy(path_input, sizeof(path_input), path_user);
            if (!path_util::build_absolute_path(proc->cwd,
                                                path_input,
                                                resolved_exec)) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }

            loader::ProgramImage image{};
            if (!load_program_image(resolved_exec, image)) {
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
            char child_cwd_buffer[path_util::kMaxPathLength];
            bool child_cwd_valid = false;
            if (cwd_user != nullptr) {
                size_t cwd_len = string_util::length(cwd_user);
                if (cwd_len > 0 && cwd_len < path_util::kMaxPathLength) {
                    char cwd_input[path_util::kMaxPathLength];
                    string_util::copy(cwd_input, sizeof(cwd_input), cwd_user);
                    child_cwd_valid = path_util::build_absolute_path(proc->cwd,
                                                                     cwd_input,
                                                                     child_cwd_buffer);
                }
            }
            if (!child_cwd_valid) {
                string_util::copy(child_cwd_buffer,
                                  sizeof(child_cwd_buffer),
                                  proc->cwd);
            }
            string_util::copy(child->cwd, sizeof(child->cwd), child_cwd_buffer);

            if (!loader::load_into_process(image, *child)) {
                child->state = process::State::Unused;
                child->pid = 0;
                child->parent = nullptr;
                child->waiting_on = nullptr;
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }

            uint64_t arg_ptr = place_args_on_stack(*child, args);

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

            proc->waiting_on = child;
            proc->state = process::State::Blocked;
            bool transferred = descriptor::transfer_console_owner(*proc, *child);
            proc->console_transferred = transferred;
            frame.rax = 0;
            return Result::Reschedule;
        }
        case SystemCall::Child: {
            process::Process* proc = process::current();
            if (proc == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }

            const char* path_user = reinterpret_cast<const char*>(frame.rdi);
            const char* args = reinterpret_cast<const char*>(frame.rsi);
            uint64_t flags = frame.rdx;
            const char* cwd_user = reinterpret_cast<const char*>(frame.r10);
            (void)flags;

            if (path_user == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }

            size_t path_len = string_util::length(path_user);
            if (path_len == 0 || path_len >= path_util::kMaxPathLength) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }

            char path_input[path_util::kMaxPathLength];
            char resolved_exec[path_util::kMaxPathLength];
            string_util::copy(path_input, sizeof(path_input), path_user);
            if (!path_util::build_absolute_path(proc->cwd,
                                                path_input,
                                                resolved_exec)) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }

            loader::ProgramImage image{};
            if (!load_program_image(resolved_exec, image)) {
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
            char child_cwd_buffer[path_util::kMaxPathLength];
            bool child_cwd_valid = false;
            if (cwd_user != nullptr) {
                size_t cwd_len = string_util::length(cwd_user);
                if (cwd_len > 0 && cwd_len < path_util::kMaxPathLength) {
                    char cwd_input[path_util::kMaxPathLength];
                    string_util::copy(cwd_input, sizeof(cwd_input), cwd_user);
                    child_cwd_valid = path_util::build_absolute_path(proc->cwd,
                                                                     cwd_input,
                                                                     child_cwd_buffer);
                }
            }
            if (!child_cwd_valid) {
                string_util::copy(child_cwd_buffer,
                                  sizeof(child_cwd_buffer),
                                  proc->cwd);
            }
            string_util::copy(child->cwd, sizeof(child->cwd), child_cwd_buffer);

            if (!loader::load_into_process(image, *child)) {
                child->state = process::State::Unused;
                child->pid = 0;
                child->parent = nullptr;
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }

            uint64_t arg_ptr = place_args_on_stack(*child, args);

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
        case SystemCall::ProcessSetCwd: {
            process::Process* proc = process::current();
            if (proc == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }

            const char* path_user = reinterpret_cast<const char*>(frame.rdi);
            if (path_user == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }

            size_t path_len = string_util::length(path_user);
            if (path_len == 0 || path_len >= path_util::kMaxPathLength) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }

            char path_input[path_util::kMaxPathLength];
            char resolved[path_util::kMaxPathLength];
            string_util::copy(path_input, sizeof(path_input), path_user);
            if (!path_util::build_absolute_path(proc->cwd,
                                                path_input,
                                                resolved)) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }

            string_util::copy(proc->cwd, sizeof(proc->cwd), resolved);
            frame.rax = 0;
            return Result::Continue;
        }
        case SystemCall::ProcessGetCwd: {
            process::Process* proc = process::current();
            if (proc == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }

            char* buffer = reinterpret_cast<char*>(frame.rdi);
            uint64_t buffer_size = frame.rsi;
            if (buffer == nullptr || buffer_size == 0) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }

            size_t cwd_len = string_util::length(proc->cwd);
            if (cwd_len + 1 > buffer_size) {
                if (buffer_size == 0) {
                    frame.rax = static_cast<uint64_t>(-1);
                    return Result::Continue;
                }
                cwd_len = static_cast<size_t>(buffer_size - 1);
            }

            for (size_t i = 0; i < cwd_len; ++i) {
                buffer[i] = proc->cwd[i];
            }
            buffer[cwd_len] = '\0';
            frame.rax = static_cast<uint64_t>(cwd_len);
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
