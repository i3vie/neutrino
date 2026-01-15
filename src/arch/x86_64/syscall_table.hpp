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
    DescriptorRead   = 5,
    DescriptorWrite  = 6,
    DescriptorClose  = 7,
    DescriptorGetType  = 8,
    DescriptorTestFlag = 9,
    DescriptorGetFlags    = 10,
    DescriptorGetProperty = 11,
    DescriptorSetProperty = 12,
    FileOpen              = 13,
    FileClose             = 14,
    FileRead              = 15,
    FileWrite             = 16,
    FileCreate            = 17,
    ProcessExec           = 18,
    Child                 = 19,
    ProcessSetCwd         = 20,
    ProcessGetCwd         = 21,
    DirectoryOpen         = 22,
    DirectoryRead         = 23,
    DirectoryClose        = 24,
    MapAnonymous          = 25,
    MapAt                 = 26,
    Unmap                 = 27,
    ChangeSlot            = 28,
    DirectoryOpenRoot     = 29,
    DirectoryOpenAt       = 30,
    FileOpenAt            = 31,
    FileCreateAt          = 32,
};

Result handle_syscall(SyscallFrame& frame);

}  // namespace syscall
