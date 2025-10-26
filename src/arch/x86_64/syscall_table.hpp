#pragma once

#include <stdint.h>

#include "arch/x86_64/syscall.hpp"

namespace syscall {

enum class SystemCall : uint64_t {
    AbiMajor         = 0,
    AbiMinor         = 1,
    Yield            = 2,
    DescriptorOpen   = 3,
    DescriptorQuery  = 4,
    DescriptorRead   = 5,
    DescriptorWrite  = 6,
    DescriptorClose  = 7,
};

Result handle_syscall(SyscallFrame& frame);

}  // namespace syscall
