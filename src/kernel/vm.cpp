#include "vm.hpp"

#include "arch/x86_64/memory/paging.hpp"
#include "lib/mem.hpp"

namespace {

constexpr uint64_t kPageSize = 0x1000;
constexpr uint64_t kPageMask = kPageSize - 1;

constexpr uint64_t kUserCodeBase = 0x0000000040000000ull;
constexpr uint64_t kUserStackCeiling = 0x00007ffffff00000ull;

uint64_t g_next_user_code = kUserCodeBase;
uint64_t g_next_user_stack = kUserStackCeiling;

constexpr uint64_t align_up(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

constexpr uint64_t align_down(uint64_t value, uint64_t alignment) {
    return value & ~(alignment - 1);
}

}  // namespace

namespace vm {

Region map_user_code(const uint8_t* data, size_t length,
                     uint64_t entry_offset, uint64_t& entry_point) {
    Region region{0, 0};
    if (data == nullptr || length == 0) {
        entry_point = 0;
        return region;
    }

    uint64_t base = align_up(g_next_user_code, kPageSize);
    size_t padded = static_cast<size_t>(align_up(length, kPageSize));
    size_t pages = padded / kPageSize;

    for (size_t i = 0; i < pages; ++i) {
        auto page = static_cast<uint8_t*>(paging_alloc_page());
        if (page == nullptr) {
            entry_point = 0;
            return Region{0, 0};
        }
        uint64_t phys = paging_virt_to_phys(reinterpret_cast<uint64_t>(page));
        uint64_t virt = base + static_cast<uint64_t>(i) * kPageSize;
        paging_map_page(virt, phys, PAGE_FLAG_WRITE | PAGE_FLAG_USER);

        size_t offset = i * kPageSize;
        size_t remaining = (offset < length) ? (length - offset) : 0;
        size_t copy_len = (remaining > kPageSize) ? kPageSize : remaining;
        if (copy_len > 0) {
            memcpy(page, data + offset, copy_len);
        }
        if (copy_len < kPageSize) {
            memset(page + copy_len, 0, kPageSize - copy_len);
        }
    }

    region.base = base;
    region.length = pages * kPageSize;
    g_next_user_code = region.base + region.length;

    uint64_t safe_offset = (entry_offset < length) ? entry_offset : 0;
    entry_point = region.base + safe_offset;
    return region;
}

Stack allocate_user_stack(size_t length) {
    if (length == 0) {
        length = kPageSize;
    }
    size_t total = static_cast<size_t>(align_up(length, kPageSize));
    size_t pages = total / kPageSize;

    uint64_t top = align_down(g_next_user_stack, kPageSize);
    uint64_t base = top - static_cast<uint64_t>(total);

    for (size_t i = 0; i < pages; ++i) {
        auto page = static_cast<uint8_t*>(paging_alloc_page());
        if (page == nullptr) {
            return Stack{0, 0, 0};
        }
        uint64_t phys = paging_virt_to_phys(reinterpret_cast<uint64_t>(page));
        uint64_t virt =
            base + static_cast<uint64_t>(i) * kPageSize;
        paging_map_page(virt, phys, PAGE_FLAG_WRITE | PAGE_FLAG_USER);
        memset(page, 0, kPageSize);
    }

    g_next_user_stack = base;
    return Stack{base, top, total};
}

}  // namespace vm

