#pragma once

#include <stdint.h>

namespace cpu {

inline uint64_t read_msr(uint32_t msr) {
    uint32_t low = 0;
    uint32_t high = 0;
    asm volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return (static_cast<uint64_t>(high) << 32) | low;
}

inline void write_msr(uint32_t msr, uint64_t value) {
    uint32_t low = static_cast<uint32_t>(value & 0xFFFFFFFFu);
    uint32_t high = static_cast<uint32_t>(value >> 32);
    asm volatile("wrmsr" : : "c"(msr), "a"(low), "d"(high));
}

constexpr uint32_t kMsrFsBase = 0xC0000100;

inline uint64_t read_fs_base() {
    return read_msr(kMsrFsBase);
}

inline void write_fs_base(uint64_t value) {
    write_msr(kMsrFsBase, value);
}

}  // namespace cpu
