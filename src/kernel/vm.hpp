#pragma once

#include <stddef.h>
#include <stdint.h>

namespace vm {

struct Region {
    uint64_t base;
    size_t length;
};

struct Stack {
    uint64_t base;
    uint64_t top;
    size_t length;
};

Region map_user_code(const uint8_t* data, size_t length,
                     uint64_t entry_offset, uint64_t& entry_point);
Region allocate_user_region(size_t length);
Stack allocate_user_stack(size_t length);
void release_user_region(const Region& region);

inline constexpr uint64_t kUserAddressSpaceBase = 0x0000000040000000ull;
inline constexpr uint64_t kUserAddressSpaceTop = 0x00007ffffff00000ull;

bool is_user_range(uint64_t address, uint64_t length);
bool copy_user_string(const char* user, char* dest, size_t dest_size);

}  // namespace vm
