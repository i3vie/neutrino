#include "arch/x86_64/pat.hpp"

#include <stdint.h>

#include "drivers/log/logging.hpp"

namespace cpu {
namespace {

struct CpuidResult {
    uint32_t eax;
    uint32_t ebx;
    uint32_t ecx;
    uint32_t edx;
};

inline CpuidResult cpuid(uint32_t leaf, uint32_t subleaf = 0) {
    CpuidResult result{};
#if defined(__GNUC__) || defined(__clang__)
    asm volatile("cpuid"
                 : "=a"(result.eax), "=b"(result.ebx),
                   "=c"(result.ecx), "=d"(result.edx)
                 : "a"(leaf), "c"(subleaf));
#else
#error "cpuid requires inline assembly"
#endif
    return result;
}

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

constexpr uint32_t IA32_PAT = 0x277;

}  // namespace

bool configure_pat_write_combining(uint8_t entry_index) {
    CpuidResult basic = cpuid(1);
    if ((basic.edx & (1u << 16)) == 0) {
        log_message(LogLevel::Warn, "PAT: CPU does not report PAT support");
        return false;
    }

    if (entry_index >= 8) {
        entry_index = 4;
    }

    uint64_t pat = read_msr(IA32_PAT);
    uint8_t current_entry = static_cast<uint8_t>((pat >> (entry_index * 8)) & 0xFFu);
    if (current_entry == 0x01u) {
        log_message(LogLevel::Info,
                    "PAT: entry %u already configured for write combining",
                    static_cast<unsigned int>(entry_index));
        return true;
    }

    pat &= ~(0xFFull << (entry_index * 8));
    pat |= (static_cast<uint64_t>(0x01u) << (entry_index * 8));
    write_msr(IA32_PAT, pat);

    log_message(LogLevel::Info,
                "PAT: entry %u set to write-combining",
                static_cast<unsigned int>(entry_index));
    return true;
}

}  // namespace cpu
