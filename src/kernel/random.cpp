#include "random.hpp"

#include <stddef.h>
#include <stdint.h>

namespace {

uint64_t g_state = 0;

bool rdrand_supported() {
    uint32_t eax = 1;
    uint32_t ebx = 0;
    uint32_t ecx = 0;
    uint32_t edx = 0;
    asm volatile("cpuid"
                 : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
    return (ecx & (1u << 30)) != 0;
}

bool rdrand32(uint32_t& value) {
    unsigned char ok = 0;
    asm volatile("rdrand %0; setc %1" : "=r"(value), "=qm"(ok));
    return ok != 0;
}

bool rdrand64(uint64_t& value) {
    unsigned char ok = 0;
    asm volatile("rdrand %0; setc %1" : "=r"(value), "=qm"(ok));
    return ok != 0;
}

uint64_t read_tsc() {
    uint32_t low = 0;
    uint32_t high = 0;
    asm volatile("rdtsc" : "=a"(low), "=d"(high));
    return (static_cast<uint64_t>(high) << 32) | low;
}

uint64_t fallback_value() {
    uint64_t observed = __atomic_load_n(&g_state, __ATOMIC_RELAXED);
    for (;;) {
        uint64_t state = observed;
        if (state == 0) {
            state = read_tsc() ^ reinterpret_cast<uintptr_t>(&g_state) ^
                    0x9E3779B97F4A7C15ull;
        }
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        state *= 0x2545F4914F6CDD1Dull;
        if (__atomic_compare_exchange_n(&g_state,
                                        &observed,
                                        state,
                                        false,
                                        __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED)) {
            return state;
        }
    }
}

}  // namespace

namespace kernel_random {

bool secure_available() {
    return rdrand_supported();
}

bool secure_fill(void* output, size_t length) {
    if (length == 0) {
        return true;
    }
    if (output == nullptr || !secure_available()) {
        return false;
    }
    auto* bytes = static_cast<uint8_t*>(output);
    size_t offset = 0;
    while (offset < length) {
        uint64_t word = 0;
        bool generated = false;
        for (size_t attempt = 0; attempt < 16; ++attempt) {
            if (rdrand64(word)) {
                generated = true;
                break;
            }
            asm volatile("pause");
        }
        if (!generated) {
            return false;
        }
        size_t chunk = length - offset;
        if (chunk > sizeof(word)) {
            chunk = sizeof(word);
        }
        for (size_t i = 0; i < chunk; ++i) {
            bytes[offset + i] = static_cast<uint8_t>(word >> (i * 8));
        }
        offset += chunk;
    }
    return true;
}

uint32_t opaque_id() {
    if (rdrand_supported()) {
        for (size_t attempt = 0; attempt < 16; ++attempt) {
            uint32_t value = 0;
            if (rdrand32(value) && value != 0) {
                return value;
            }
            asm volatile("pause");
        }
    }
    uint32_t value = static_cast<uint32_t>(fallback_value());
    return value != 0 ? value : 1u;
}

}  // namespace kernel_random
