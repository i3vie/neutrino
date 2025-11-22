// Simple no-op utility to exercise the scheduler.
// Build via userspace/Makefile and run as `noop`.

#include <stddef.h>
#include <stdint.h>

#include "../crt/syscall.hpp"

int main(uint64_t, uint64_t) {
    // Burn a small, deterministic amount of CPU time.
    volatile uint64_t acc = 0;
    for (uint64_t i = 0; i < 500000; ++i) {
        acc += i;
        asm volatile("pause");
    }
    (void)acc;
    return 0;
}
