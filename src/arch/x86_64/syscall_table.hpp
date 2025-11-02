#pragma once

#include <stdint.h>

#include "arch/x86_64/syscall.hpp"

namespace syscall {

enum class SystemCall : uint64_t {
    AbiMajor         = 0,
    AbiMinor         = 1,
    Exit             = 2,
    Yield            = 3,
    DescriptorOpen   = 4,
    DescriptorQuery  = 5,
    DescriptorRead   = 6,
    DescriptorWrite  = 7,
    DescriptorClose  = 8,
    FileOpen         = 9,
    FileClose        = 10,
    FileRead         = 11,
    FileWrite        = 12,
    FileCreate       = 13,
    ProcessExec      = 14,
    Child            = 15,
    ProcessSetCwd    = 16,
    ProcessGetCwd    = 17,
    DirectoryOpen    = 18,
    DirectoryRead    = 19,
    DirectoryClose   = 20,
};

Result handle_syscall(SyscallFrame& frame);

}  // namespace syscall
