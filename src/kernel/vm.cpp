#include "vm.hpp"

#include "arch/x86_64/memory/paging.hpp"
#include "lib/mem.hpp"

namespace {

constexpr uint64_t kPageSize = 0x1000;
constexpr uint64_t kPageMask = kPageSize - 1;

constexpr uint64_t kUserCodeBase = vm::kUserAddressSpaceBase;
constexpr uint64_t kUserStackCeiling = vm::kUserAddressSpaceTop;

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

Region allocate_user_region(size_t length) {
    Region region{0, 0};
    if (length == 0) {
        return region;
    }

    uint64_t base = align_up(g_next_user_code, kPageSize);
    size_t padded = static_cast<size_t>(align_up(length, kPageSize));
    size_t pages = padded / kPageSize;

    for (size_t i = 0; i < pages; ++i) {
        auto page = static_cast<uint8_t*>(paging_alloc_page());
        if (page == nullptr) {
            return Region{0, 0};
        }
        memset(page, 0, kPageSize);
        uint64_t phys = paging_virt_to_phys(reinterpret_cast<uint64_t>(page));
        uint64_t virt = base + static_cast<uint64_t>(i) * kPageSize;
        paging_map_page(virt, phys, PAGE_FLAG_WRITE | PAGE_FLAG_USER);
    }

    region.base = base;
    region.length = pages * kPageSize;
    g_next_user_code = region.base + region.length;
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

void release_user_region(const Region& region) {
    if (region.base == 0 || region.length == 0) {
        return;
    }
    uint64_t base = align_down(region.base, kPageSize);
    uint64_t limit = align_up(region.length, kPageSize);
    for (uint64_t offset = 0; offset < limit; offset += kPageSize) {
        uint64_t virt = base + offset;
        uint64_t phys = 0;
        if (!paging_unmap_page(virt, phys)) {
            continue;
        }
        paging_free_physical(phys);
    }
}

bool is_user_range(uint64_t address, uint64_t length) {
    if (address < kUserAddressSpaceBase ||
        address >= kUserAddressSpaceTop) {
        return false;
    }
    if (length == 0) {
        return true;
    }
    uint64_t max_len = kUserAddressSpaceTop - address;
    if (length > max_len) {
        return false;
    }
    return true;
}

bool copy_user_string(const char* user, char* dest, size_t dest_size) {
    if (dest == nullptr || dest_size == 0) {
        return false;
    }
    dest[0] = '\0';
    if (user == nullptr) {
        return false;
    }
    size_t idx = 0;
    while (idx + 1 < dest_size) {
        uint64_t addr = reinterpret_cast<uint64_t>(user + idx);
        if (!is_user_range(addr, 1)) {
            dest[0] = '\0';
            return false;
        }
        char ch = user[idx];
        dest[idx++] = ch;
        if (ch == '\0') {
            return true;
        }
    }
    dest[dest_size - 1] = '\0';
    return false;
}

}  // namespace vm
