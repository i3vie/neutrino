#pragma once

#include <stddef.h>
#include <stdint.h>

enum class SystemCall : long {
    AbiMajor         = 0,
    AbiMinor         = 1,
    Exit             = 2,
    Yield            = 3,
    DescriptorOpen   = 4,
    DescriptorQuery  = 5,
    DescriptorRead   = 6,
    DescriptorWrite  = 7,
    DescriptorClose  = 8,
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

// requests a descriptor of the given type. the optional parameters allow
// callers to select a specific resource instance, request particular
// capability bits, or pass type-specific context understood by the kernel
// or provider.
static inline long descriptor_open(uint32_t type,
                                   uint64_t resource_selector = 0,
                                   uint64_t requested_capabilities = 0,
                                   uint64_t open_context = 0) {
    return raw_syscall4(SystemCall::DescriptorOpen,
                        static_cast<long>(type),
                        static_cast<long>(resource_selector),
                        static_cast<long>(requested_capabilities),
                        static_cast<long>(open_context));
}

static inline long descriptor_query(uint32_t handle) {
    return raw_syscall1(SystemCall::DescriptorQuery,
                        static_cast<long>(handle));
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
