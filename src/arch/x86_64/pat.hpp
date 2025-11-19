#pragma once

#include <stdint.h>

namespace cpu {

// Configure a PAT entry (default index 4) to use the write-combining memory
// type. Returns true on success.
bool configure_pat_write_combining(uint8_t entry_index = 4);

}  // namespace cpu
