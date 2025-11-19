#pragma once

#include <stdint.h>

namespace cpu {

// attempts to configure one or more MTRR variable ranges so that the given
// physical address range is mapped as write-combining. returns true on success
bool configure_write_combining(uint64_t phys_base, uint64_t length);

}  // namespace cpu
