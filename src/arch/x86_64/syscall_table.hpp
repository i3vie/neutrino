#pragma once

#include <stdint.h>

#include "arch/x86_64/syscall.hpp"

namespace syscall {

enum class SystemCall : uint64_t {
    AbiMajor             = 0,
    AbiMinor             = 1,
    Exit                 = 2,
    Yield                = 3,
    Sleep                = 4,
    DescriptorOpen       = 5,
    DescriptorRead       = 6,
    DescriptorWrite      = 7,
    DescriptorClose      = 8,
    DescriptorGetType    = 9,
    DescriptorTestFlag   = 10,
    DescriptorGetFlags   = 11,
    DescriptorGetProperty= 12,
    DescriptorSetProperty= 13,
    DescriptorWait       = 14,
    FileOpen             = 15,
    FileClose            = 16,
    FileRead             = 17,
    FileWrite            = 18,
    FileCreate           = 19,
    FileOpenAt           = 20,
    FileCreateAt         = 21,
    FileRemove           = 22,
    DirectoryOpen        = 23,
    DirectoryRead        = 24,
    DirectoryClose       = 25,
    DirectoryOpenRoot    = 26,
    DirectoryOpenAt      = 27,
    DirectoryCreate      = 28,
    DirectoryRemove      = 29,
    ProcessExec          = 30,
    Child                = 31,
    ProcessSetCwd        = 32,
    ProcessGetCwd        = 33,
    MapAnonymous         = 34,
    MapAt                = 35,
    Unmap                = 36,
    ChangeSlot           = 37,
    TimeGet              = 38,
    Mount                = 39,
    RescanBlockDevices   = 40,
    PrincipalCreate      = 41,
    PrincipalSet         = 42,
    CapabilityGrant      = 43,
    CapabilityPass       = 44,
    UserCreate           = 45,
    UserFind             = 46,
    UserBumpGeneration   = 47,
    UserSetPassword      = 48,
    UserInfo             = 49,
};

Result handle_syscall(SyscallFrame& frame);

}  // namespace syscall
