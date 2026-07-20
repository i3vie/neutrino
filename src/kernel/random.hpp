#pragma once

#include <stddef.h>
#include <stdint.h>

namespace kernel_random {

bool secure_available();

// Fills a buffer from the CPU hardware random source. Returns false without
// weakening security when no trustworthy source is available.
bool secure_fill(void* output, size_t length);

// Returns an unpredictable non-zero value when RDRAND is available.  The
// mixed fallback is intended for opaque object identifiers, not key material.
uint32_t opaque_id();

}  // namespace kernel_random
