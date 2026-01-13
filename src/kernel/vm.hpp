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

enum MapFlags : uint64_t {
    kMapWrite = 1ull << 0,
};

Region map_user_code(uint64_t cr3,
                     const uint8_t* data,
                     size_t length,
                     uint64_t entry_offset, uint64_t& entry_point);
Region reserve_user_region(size_t length);
Region allocate_user_region(uint64_t cr3, size_t length);
Stack allocate_user_stack(uint64_t cr3, size_t length);
void release_user_region(uint64_t cr3, const Region& region);
uint64_t map_anonymous(uint64_t cr3, size_t length, uint64_t flags);
uint64_t map_at(uint64_t cr3, uint64_t addr_hint, size_t length, uint64_t flags);
bool unmap_region(uint64_t cr3, uint64_t addr, size_t length);

inline constexpr uint64_t kUserAddressSpaceBase = 0x0000000040000000ull;
inline constexpr uint64_t kUserAddressSpaceTop = 0x00007ffffff00000ull;

bool is_user_range(uint64_t address, uint64_t length);
bool copy_user_string(const char* user, char* dest, size_t dest_size);
bool copy_to_user(uint64_t cr3, uint64_t dest, const void* src, size_t length);
bool copy_from_user(uint64_t cr3, void* dest, uint64_t src, size_t length);
bool fill_user(uint64_t cr3, uint64_t dest, uint8_t value, size_t length);

}  // namespace vm
