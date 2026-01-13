#pragma once

#include <stddef.h>
#include <stdint.h>

#include "descriptors.hpp"

enum class SystemCall : long {
    AbiMajor         = 0,
    AbiMinor         = 1,
    Exit             = 2,
    Yield            = 3,
    DescriptorOpen   = 4,
    DescriptorRead   = 5,
    DescriptorWrite      = 6,
    DescriptorClose      = 7,
    DescriptorGetType    = 8,
    DescriptorTestFlag   = 9,
    DescriptorGetFlags   = 10,
    DescriptorGetProperty= 11,
    DescriptorSetProperty= 12,
    FileOpen             = 13,
    FileClose            = 14,
    FileRead             = 15,
    FileWrite            = 16,
    FileCreate           = 17,
    ProcessExec          = 18,
    Child                = 19,
    ProcessSetCwd        = 20,
    ProcessGetCwd        = 21,
    DirectoryOpen        = 22,
    DirectoryRead        = 23,
    DirectoryClose       = 24,
};

enum : uint32_t {
    DIR_ENTRY_FLAG_DIRECTORY = 1u << 0,
};

constexpr uint32_t kInvalidDescriptor = 0xFFFFFFFFu;

struct DirEntry {
    char name[64];
    uint32_t flags;
    uint32_t reserved;
    uint64_t size;
};

static inline long raw_syscall6(SystemCall num,
                                long a1,
                                long a2,
                                long a3,
                                long a4,
                                long a5,
                                long a6) {
    long ret;
    long cn = static_cast<long>(num);
    asm volatile("mov %5, %%r10\n\t"
                 "mov %6, %%r8\n\t"
                 "mov %7, %%r9\n\t"
                 "syscall"
                 : "=a"(ret)
                 : "a"(cn), "D"(a1), "S"(a2), "d"(a3),
                   "r"(a4), "r"(a5), "r"(a6)
                 : "rcx", "r11", "r10", "r8", "r9", "memory");
    return ret;
}

static inline long raw_syscall5(SystemCall num,
                                long a1,
                                long a2,
                                long a3,
                                long a4,
                                long a5) {
    return raw_syscall6(num, a1, a2, a3, a4, a5, 0);
}

static inline long raw_syscall4(SystemCall num,
                                long a1,
                                long a2,
                                long a3,
                                long a4) {
    return raw_syscall6(num, a1, a2, a3, a4, 0, 0);
}

static inline long raw_syscall3(SystemCall num,
                                long a1,
                                long a2,
                                long a3) {
    return raw_syscall6(num, a1, a2, a3, 0, 0, 0);
}

static inline long raw_syscall2(SystemCall num,
                                long a1,
                                long a2) {
    return raw_syscall6(num, a1, a2, 0, 0, 0, 0);
}

static inline long raw_syscall1(SystemCall num,
                                long a1) {
    return raw_syscall6(num, a1, 0, 0, 0, 0, 0);
}

static inline long raw_syscall0(SystemCall num) {
    return raw_syscall6(num, 0, 0, 0, 0, 0, 0);
}

static inline long abi_major() {
    return raw_syscall0(SystemCall::AbiMajor);
}

static inline long abi_minor() {
    return raw_syscall0(SystemCall::AbiMinor);
}

[[noreturn]] static inline void exit(uint16_t code) {
    raw_syscall1(SystemCall::Exit, static_cast<long>(code));
    __builtin_unreachable();
}

static inline long yield() {
    return raw_syscall0(SystemCall::Yield);
}

static inline long child(const char* path,
                         const char* args,
                         uint64_t flags,
                         const char* cwd) {
    return raw_syscall4(SystemCall::Child,
                        static_cast<long>(reinterpret_cast<uintptr_t>(path)),
                        static_cast<long>(reinterpret_cast<uintptr_t>(args)),
                        static_cast<long>(flags),
                        static_cast<long>(reinterpret_cast<uintptr_t>(cwd)));
}

// requests a descriptor of the given type. the optional parameters allow
// callers to select a specific resource instance, request particular
// flag bits, or pass type-specific context understood by the kernel
// or provider.
static inline long descriptor_open(uint32_t type,
                                   uint64_t resource_selector = 0,
                                   uint64_t requested_flags = 0,
                                   uint64_t open_context = 0) {
    return raw_syscall4(SystemCall::DescriptorOpen,
                        static_cast<long>(type),
                        static_cast<long>(resource_selector),
                        static_cast<long>(requested_flags),
                        static_cast<long>(open_context));
}

static inline long descriptor_get_type(uint32_t handle) {
    return raw_syscall1(SystemCall::DescriptorGetType,
                        static_cast<long>(handle));
}

static inline long descriptor_test_flag(uint32_t handle, uint64_t flag) {
    return raw_syscall2(SystemCall::DescriptorTestFlag,
                        static_cast<long>(handle),
                        static_cast<long>(flag));
}

static inline long descriptor_get_flags(uint32_t handle, bool extended) {
    return raw_syscall2(SystemCall::DescriptorGetFlags,
                        static_cast<long>(handle),
                        static_cast<long>(extended ? 1 : 0));
}

static inline long descriptor_get_property(uint32_t handle,
                                           uint32_t property,
                                           void* out,
                                           size_t size) {
    return raw_syscall4(SystemCall::DescriptorGetProperty,
                        static_cast<long>(handle),
                        static_cast<long>(property),
                        static_cast<long>(reinterpret_cast<uintptr_t>(out)),
                        static_cast<long>(size));
}

static inline long descriptor_set_property(uint32_t handle,
                                           uint32_t property,
                                           const void* in,
                                           size_t size) {
    return raw_syscall4(SystemCall::DescriptorSetProperty,
                        static_cast<long>(handle),
                        static_cast<long>(property),
                        static_cast<long>(reinterpret_cast<uintptr_t>(in)),
                        static_cast<long>(size));
}

static inline long shared_memory_open(const char* name, size_t length) {
    return descriptor_open(
        static_cast<uint32_t>(descriptor_defs::Type::SharedMemory),
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(name)),
        static_cast<uint64_t>(length),
        0);
}

static inline long shared_memory_get_info(
    uint32_t handle,
    descriptor_defs::SharedMemoryInfo* info) {
    if (info == nullptr) {
        return -1;
    }
    return descriptor_get_property(
        handle,
        static_cast<uint32_t>(descriptor_defs::Property::SharedMemoryInfo),
        info,
        sizeof(*info));
}

static inline long framebuffer_open() {
    return descriptor_open(
        static_cast<uint32_t>(descriptor_defs::Type::Framebuffer),
        0,
        0,
        0);
}

static inline long framebuffer_open_slot(uint32_t slot) {
    return descriptor_open(
        static_cast<uint32_t>(descriptor_defs::Type::Framebuffer),
        static_cast<uint64_t>(slot),
        0,
        0);
}

static inline long framebuffer_get_info(
    uint32_t handle,
    descriptor_defs::FramebufferInfo* info) {
    if (info == nullptr) {
        return -1;
    }
    return descriptor_get_property(
        handle,
        static_cast<uint32_t>(descriptor_defs::Property::FramebufferInfo),
        info,
        sizeof(*info));
}

static inline long framebuffer_present(
    uint32_t handle,
    const descriptor_defs::FramebufferRect* rect) {
    return descriptor_set_property(
        handle,
        static_cast<uint32_t>(descriptor_defs::Property::FramebufferPresent),
        rect,
        rect ? sizeof(*rect) : 0);
}

static inline long mouse_open() {
    return descriptor_open(
        static_cast<uint32_t>(descriptor_defs::Type::Mouse),
        0,
        0,
        0);
}

static inline long pipe_open_new(uint64_t flags) {
    return descriptor_open(
        static_cast<uint32_t>(descriptor_defs::Type::Pipe),
        flags,
        0,
        0);
}

static inline long pipe_open_existing(uint64_t flags, uint64_t pipe_id) {
    return descriptor_open(
        static_cast<uint32_t>(descriptor_defs::Type::Pipe),
        flags,
        pipe_id,
        0);
}

static inline long pipe_get_info(uint32_t handle,
                                 descriptor_defs::PipeInfo* info) {
    if (info == nullptr) {
        return -1;
    }
    return descriptor_get_property(
        handle,
        static_cast<uint32_t>(descriptor_defs::Property::PipeInfo),
        info,
        sizeof(*info));
}

static inline long descriptor_read(uint32_t handle,
                                       void* buffer,
                                       size_t length,
                                       uint64_t offset = 0) {
    return raw_syscall4(SystemCall::DescriptorRead,
                        static_cast<long>(handle),
                        static_cast<long>(reinterpret_cast<uintptr_t>(buffer)),
                        static_cast<long>(length),
                        static_cast<long>(offset));
}

static inline long descriptor_write(uint32_t handle,
                                        const void* buffer,
                                        size_t length,
                                        uint64_t offset = 0) {
    return raw_syscall4(SystemCall::DescriptorWrite,
                        static_cast<long>(handle),
                        static_cast<long>(reinterpret_cast<uintptr_t>(buffer)),
                        static_cast<long>(length),
                        static_cast<long>(offset));
}

static inline long descriptor_close(uint32_t handle) {
    return raw_syscall1(SystemCall::DescriptorClose,
                        static_cast<long>(handle));
}

static inline long file_open(const char* path) {
    return raw_syscall1(SystemCall::FileOpen,
                        static_cast<long>(reinterpret_cast<uintptr_t>(path)));
}

static inline long file_close(uint32_t handle) {
    return raw_syscall1(SystemCall::FileClose,
                        static_cast<long>(handle));
}

static inline long file_read(uint32_t handle,
                             void* buffer,
                             size_t length) {
    return raw_syscall3(SystemCall::FileRead,
                        static_cast<long>(handle),
                        static_cast<long>(reinterpret_cast<uintptr_t>(buffer)),
                        static_cast<long>(length));
}

static inline long file_write(uint32_t handle,
                              const void* buffer,
                              size_t length) {
    return raw_syscall3(SystemCall::FileWrite,
                        static_cast<long>(handle),
                        static_cast<long>(reinterpret_cast<uintptr_t>(buffer)),
                        static_cast<long>(length));
}

static inline long exec(const char* path,
                        const char* args,
                        uint64_t flags,
                        const char* cwd) {
    return raw_syscall4(SystemCall::ProcessExec,
                        static_cast<long>(reinterpret_cast<uintptr_t>(path)),
                        static_cast<long>(reinterpret_cast<uintptr_t>(args)),
                        static_cast<long>(flags),
                        static_cast<long>(reinterpret_cast<uintptr_t>(cwd)));
}

static inline long setcwd(const char* path) {
    return raw_syscall1(SystemCall::ProcessSetCwd,
                        static_cast<long>(reinterpret_cast<uintptr_t>(path)));
}

static inline long getcwd(char* buffer, size_t length) {
    return raw_syscall2(SystemCall::ProcessGetCwd,
                        static_cast<long>(reinterpret_cast<uintptr_t>(buffer)),
                        static_cast<long>(length));
}

static inline long directory_open(const char* path) {
    return raw_syscall1(SystemCall::DirectoryOpen,
                        static_cast<long>(reinterpret_cast<uintptr_t>(path)));
}

static inline long file_create(const char* path) {
    return raw_syscall1(SystemCall::FileCreate,
                        static_cast<long>(reinterpret_cast<uintptr_t>(path)));
}

static inline long directory_read(uint32_t handle, DirEntry* out_entry) {
    return raw_syscall2(SystemCall::DirectoryRead,
                        static_cast<long>(handle),
                        static_cast<long>(reinterpret_cast<uintptr_t>(out_entry)));
}

static inline long directory_close(uint32_t handle) {
    return raw_syscall1(SystemCall::DirectoryClose,
                        static_cast<long>(handle));
}
