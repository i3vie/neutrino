#include "arch/x86_64/mtrr.hpp"

#include <stddef.h>
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
#error "cpuid needs inline assembly"
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

inline bool interrupts_enabled() {
    uint64_t rflags = 0;
    asm volatile("pushfq; pop %0" : "=r"(rflags));
    return (rflags & (1ull << 9)) != 0;
}

inline void set_interrupts(bool enabled) {
    if (enabled) {
        asm volatile("sti");
    } else {
        asm volatile("cli");
    }
}

constexpr uint32_t MSR_MTRR_DEF_TYPE = 0x2FF;
constexpr uint32_t MSR_MTRR_CAP = 0xFE;
constexpr uint32_t MSR_MTRR_PHYSBASE0 = 0x200;
constexpr uint32_t MSR_MTRR_PHYSMASK0 = 0x201;

constexpr uint64_t CR0_CD = 1ull << 30;
constexpr uint64_t CR0_NW = 1ull << 29;

constexpr uint64_t MEMORY_TYPE_WC = 0x01;
constexpr uint64_t PAGE_MASK = ~0xFFFull;
constexpr uint64_t DEFAULT_PHYS_MASK = 0xFFFFFFFFFFFFF000ull;
constexpr uint64_t MTRR_VALID = 1ull << 11;

struct Range {
    uint64_t base;
    uint64_t length;
};

uint64_t physical_address_mask() {
    CpuidResult max_ext = cpuid(0x80000000);
    uint32_t phys_bits = 36;
    if (max_ext.eax >= 0x80000008) {
        CpuidResult ext = cpuid(0x80000008);
        uint32_t reported = ext.eax & 0xFFu;
        if (reported != 0) {
            phys_bits = reported;
        }
    }
    if (phys_bits < 36) {
        phys_bits = 36;
    }
    if (phys_bits > 52) {
        phys_bits = 52;
    }
    uint64_t mask;
    if (phys_bits >= 52) {
        mask = DEFAULT_PHYS_MASK;
    } else {
        mask = ((1ull << phys_bits) - 1ull) & DEFAULT_PHYS_MASK;
    }
    if (mask == 0) {
        mask = DEFAULT_PHYS_MASK;
    }
    return mask;
}

size_t split_range(uint64_t base,
                   uint64_t length,
                   Range* out,
                   size_t max_ranges) {
    size_t count = 0;
    while (length > 0 && count < max_ranges) {
        uint64_t size = 1ull << (63 - __builtin_clzll(length));
        if (size > length) {
            size = length;
        }
        if (size < 0x1000) {
            size = 0x1000;
        }
        while ((base & (size - 1)) != 0 && size > 0x1000) {
            size >>= 1;
        }
        if (size < 0x1000) {
            size = 0x1000;
        }
        if (size > length) {
            size = length;
        }
        if (size == 0) {
            break;
        }
        out[count++] = Range{base, size};
        base += size;
        length -= size;
    }
    return (length == 0) ? count : 0;
}

}  // namespace

bool configure_write_combining(uint64_t phys_base, uint64_t length) {
    if (length == 0) {
        return false;
    }

    const uint64_t address_mask = physical_address_mask();
    const uint64_t highest_address = address_mask | ~PAGE_MASK;
    const uint64_t max_limit =
        (highest_address == UINT64_MAX) ? UINT64_MAX : highest_address + 1;

    uint64_t aligned_base = phys_base & PAGE_MASK;
    if ((aligned_base & ~address_mask) != 0) {
        log_message(LogLevel::Warn,
                    "MTRR: framebuffer base %016llx exceeds physical mask",
                    static_cast<unsigned long long>(aligned_base));
        return false;
    }

    uint64_t offset = phys_base - aligned_base;
    uint64_t adjusted_length;
    if (__builtin_add_overflow(length, offset, &adjusted_length)) {
        adjusted_length = UINT64_MAX;
    }

    uint64_t max_length =
        (max_limit == UINT64_MAX) ? UINT64_MAX : (max_limit - aligned_base);
    if (adjusted_length > max_length) {
        adjusted_length = max_length;
    }
    if (adjusted_length == 0) {
        return false;
    }

    if (adjusted_length > UINT64_MAX - 0xFFFull) {
        adjusted_length = UINT64_MAX & PAGE_MASK;
    } else {
        adjusted_length = (adjusted_length + 0xFFFull) & PAGE_MASK;
    }
    if (adjusted_length == 0 || adjusted_length > max_length) {
        adjusted_length = max_length & PAGE_MASK;
    }
    if (adjusted_length == 0) {
        return false;
    }

    phys_base = aligned_base;
    length = adjusted_length;

    log_message(LogLevel::Debug,
                "MTRR: requesting WC for phys=%016llx len=%llu",
                static_cast<unsigned long long>(phys_base),
                static_cast<unsigned long long>(length));

    CpuidResult basic = cpuid(1);
    if ((basic.edx & (1u << 12)) == 0) {
        log_message(LogLevel::Warn,
                    "MTRR: CPU does not support MTRRs");
        return false;
    }

    uint64_t mtrr_cap = read_msr(MSR_MTRR_CAP);
    uint32_t var_count = static_cast<uint32_t>(mtrr_cap & 0xFFu);
    if (var_count == 0) {
        log_message(LogLevel::Warn,
                    "MTRR: no variable ranges available");
        return false;
    }

    if (var_count > 16) {
        var_count = 16;
    }

    Range ranges[16];
    size_t range_count = split_range(phys_base,
                                     length,
                                     ranges,
                                     sizeof(ranges) / sizeof(ranges[0]));
    if (range_count == 0) {
        log_message(LogLevel::Warn,
                    "MTRR: failed to decompose WC range");
        return false;
    }
    if (range_count > var_count) {
        log_message(LogLevel::Warn,
                    "MTRR: insufficient variable ranges (%zu needed, %u available)",
                    range_count,
                    static_cast<unsigned int>(var_count));
        return false;
    }

    uint64_t base_values[16] = {};
    uint64_t mask_values[16] = {};
    bool entry_in_use[16] = {};

    for (uint32_t i = 0; i < var_count; ++i) {
        base_values[i] = read_msr(MSR_MTRR_PHYSBASE0 + i * 2);
        mask_values[i] = read_msr(MSR_MTRR_PHYSMASK0 + i * 2);
        entry_in_use[i] = (mask_values[i] & MTRR_VALID) != 0;
    }

    bool range_covered[16] = {};
    for (size_t r = 0; r < range_count; ++r) {
        for (uint32_t e = 0; e < var_count; ++e) {
            if (!entry_in_use[e]) {
                continue;
            }
            if ((base_values[e] & 0xFFu) != MEMORY_TYPE_WC) {
                continue;
            }
            uint64_t entry_base = base_values[e] & address_mask;
            uint64_t mask_phys = mask_values[e] & address_mask;
            uint64_t size_raw = (~mask_phys) & address_mask;
            uint64_t entry_size = (size_raw + 0x1000) & PAGE_MASK;
            if (entry_size == 0) {
                entry_size = 0x1000;
            }
            uint64_t entry_end = entry_base;
            if (!__builtin_add_overflow(entry_base, entry_size, &entry_end)) {
                uint64_t range_end = ranges[r].base + ranges[r].length;
                if (range_end < ranges[r].base) {
                    range_end = UINT64_MAX;
                }
                if (entry_base <= ranges[r].base &&
                    entry_end >= range_end) {
                    range_covered[r] = true;
                    break;
                }
            }
        }
    }

    bool entry_reserved[16];
    for (uint32_t i = 0; i < var_count; ++i) {
        entry_reserved[i] = entry_in_use[i];
    }

    uint64_t new_base_values[16] = {};
    uint64_t new_mask_values[16] = {};
    size_t program_indices[16];
    size_t program_count = 0;

    for (size_t i = 0; i < range_count; ++i) {
        if (range_covered[i]) {
            continue;
        }
        size_t chosen = SIZE_MAX;
        for (uint32_t idx = 0; idx < var_count; ++idx) {
            if (!entry_reserved[idx]) {
                chosen = idx;
                entry_reserved[idx] = true;
                break;
            }
        }
        if (chosen == SIZE_MAX) {
            log_message(LogLevel::Warn,
                        "MTRR: no free variable ranges for WC mapping");
            return false;
        }
        uint64_t base_val =
            (ranges[i].base & address_mask) | MEMORY_TYPE_WC;
        uint64_t mask_val =
            (~(ranges[i].length - 1) & address_mask) | MTRR_VALID;
        new_base_values[chosen] = base_val;
        new_mask_values[chosen] = mask_val;
        program_indices[program_count++] = chosen;
        log_message(LogLevel::Debug,
                    "MTRR: prepare WC entry %zu base=%016llx size=%llu",
                    static_cast<size_t>(chosen),
                    static_cast<unsigned long long>(ranges[i].base),
                    static_cast<unsigned long long>(ranges[i].length));
    }

    if (program_count == 0) {
        return true;
    }

    bool ints = interrupts_enabled();
    set_interrupts(false);

    uint64_t cr0 = 0;
    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    uint64_t cr0_disabled = (cr0 | CR0_CD) & ~CR0_NW;
    asm volatile("mov %0, %%cr0" : : "r"(cr0_disabled) : "memory");
    asm volatile("wbinvd");

    uint64_t def_type = read_msr(MSR_MTRR_DEF_TYPE);
    bool mtrr_enabled = (def_type & 0x800ull) != 0;
    if (mtrr_enabled) {
        write_msr(MSR_MTRR_DEF_TYPE, def_type & ~0x800ull);
    }

    for (size_t i = 0; i < program_count; ++i) {
        size_t idx = program_indices[i];
        write_msr(MSR_MTRR_PHYSMASK0 + idx * 2, 0);
        write_msr(MSR_MTRR_PHYSBASE0 + idx * 2, new_base_values[idx]);
        write_msr(MSR_MTRR_PHYSMASK0 + idx * 2, new_mask_values[idx]);
    }

    asm volatile("wbinvd");

    if (mtrr_enabled) {
        write_msr(MSR_MTRR_DEF_TYPE, def_type);
    } else {
        write_msr(MSR_MTRR_DEF_TYPE, def_type | 0x800ull);
    }

    asm volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");
    asm volatile("wbinvd");

    set_interrupts(ints);

    log_message(LogLevel::Info,
                "MTRR: configured %zu WC range(s) for framebuffer",
                program_count);
    return true;
}

}  // namespace cpu
