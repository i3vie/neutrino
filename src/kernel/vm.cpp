#include "vm.hpp"

#include "arch/x86_64/memory/paging.hpp"
#include "lib/mem.hpp"
#include "process.hpp"

namespace {

constexpr uint64_t kPageSize = 0x1000;
constexpr uint64_t kPageMask = kPageSize - 1;

constexpr uint64_t kUserCodeBase = vm::kUserAddressSpaceBase;
constexpr uint64_t kUserStackCeiling = vm::kUserAddressSpaceTop;

constexpr uint64_t kSharedStackGuard = 0x200000;

constexpr uint64_t align_up(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

constexpr uint64_t align_down(uint64_t value, uint64_t alignment) {
    return value & ~(alignment - 1);
}

struct AddressSpaceCursors {
    uint64_t next_code;
    uint64_t next_stack;
    uint64_t next_shared;
};

AddressSpaceCursors kernel_cursors{
    kUserCodeBase, kUserStackCeiling, kUserStackCeiling};

AddressSpaceCursors* resolve_cursors(uint64_t cr3) {
    if (cr3 == 0) {
        return &kernel_cursors;
    }
    process::Process* proc = process::find_by_cr3(cr3);
    if (proc == nullptr) {
        return nullptr;
    }
    return reinterpret_cast<AddressSpaceCursors*>(&proc->next_code_cursor);
}

}  // namespace

namespace vm {

void reset_address_space_cursors(uint64_t cr3) {
    AddressSpaceCursors* entry = resolve_cursors(cr3);
    if (entry != nullptr) {
        entry->next_code = kUserCodeBase;
        entry->next_stack = kUserStackCeiling;
        entry->next_shared = kUserStackCeiling;
    }
}

Region map_user_code(uint64_t cr3, const uint8_t* data, size_t length,
                     uint64_t entry_offset, uint64_t& entry_point) {
    Region region{0, 0};
    if (data == nullptr || length == 0) {
        entry_point = 0;
        return region;
    }

    AddressSpaceCursors* cursors = resolve_cursors(cr3);
    if (cursors == nullptr) {
        entry_point = 0;
        return region;
    }

    uint64_t base = align_up(cursors->next_code, kPageSize);
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
        paging_map_page_in_space(cr3, virt, phys,
                                 PAGE_FLAG_WRITE | PAGE_FLAG_USER);

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
    cursors->next_code = region.base + region.length;

    uint64_t safe_offset = (entry_offset < length) ? entry_offset : 0;
    entry_point = region.base + safe_offset;
    return region;
}

Region allocate_user_region(uint64_t cr3, size_t length) {
    Region region{0, 0};
    if (length == 0) {
        return region;
    }

    AddressSpaceCursors* cursors = resolve_cursors(cr3);
    if (cursors == nullptr) {
        return region;
    }

    uint64_t base = align_up(cursors->next_code, kPageSize);
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
        paging_map_page_in_space(cr3, virt, phys,
                                 PAGE_FLAG_WRITE | PAGE_FLAG_USER);
    }

    region.base = base;
    region.length = pages * kPageSize;
    cursors->next_code = region.base + region.length;
    return region;
}

Region allocate_user_shared_region(uint64_t cr3, size_t length) {
    Region region{0, 0};
    if (length == 0) {
        return region;
    }
    size_t padded = static_cast<size_t>(align_up(length, kPageSize));
    size_t pages = padded / kPageSize;

    AddressSpaceCursors* cursors = resolve_cursors(cr3);
    if (cursors == nullptr) {
        return region;
    }

    uint64_t limit = (cursors->next_stack <= kSharedStackGuard)
                         ? 0
                         : cursors->next_stack - kSharedStackGuard;
    uint64_t top = cursors->next_shared;
    if (top > limit) {
        top = limit;
    }
    if (top <= kUserAddressSpaceBase || top <= padded) {
        return region;
    }
    uint64_t base = align_down(top - padded, kPageSize);
    if (base < kUserAddressSpaceBase) {
        return region;
    }

    for (size_t i = 0; i < pages; ++i) {
        auto page = static_cast<uint8_t*>(paging_alloc_page());
        if (page == nullptr) {
            return Region{0, 0};
        }
        memset(page, 0, kPageSize);
        uint64_t phys = paging_virt_to_phys(reinterpret_cast<uint64_t>(page));
        uint64_t virt = base + static_cast<uint64_t>(i) * kPageSize;
        paging_map_page_in_space(cr3, virt, phys,
                                 PAGE_FLAG_WRITE | PAGE_FLAG_USER);
    }

    region.base = base;
    region.length = pages * kPageSize;
    // Keep future stack allocations below this shared region with a guard gap.
    cursors->next_shared = base;
    uint64_t guarded_stack_top = (base > kSharedStackGuard)
                                     ? base - kSharedStackGuard
                                     : kUserAddressSpaceBase;
    if (cursors->next_stack > guarded_stack_top) {
        cursors->next_stack = guarded_stack_top;
    }
    return region;
}

Stack allocate_user_stack(uint64_t cr3, size_t length) {
    if (length == 0) {
        length = kPageSize;
    }

    AddressSpaceCursors* cursors = resolve_cursors(cr3);
    if (cursors == nullptr) {
        return Stack{0, 0, 0};
    }

    size_t total = static_cast<size_t>(align_up(length, kPageSize));
    size_t pages = total / kPageSize;

    uint64_t top = align_down(cursors->next_stack, kPageSize);
    uint64_t base = top - static_cast<uint64_t>(total);

    for (size_t i = 0; i < pages; ++i) {
        auto page = static_cast<uint8_t*>(paging_alloc_page());
        if (page == nullptr) {
            return Stack{0, 0, 0};
        }
        uint64_t phys = paging_virt_to_phys(reinterpret_cast<uint64_t>(page));
        uint64_t virt = base + static_cast<uint64_t>(i) * kPageSize;
        paging_map_page_in_space(cr3, virt, phys,
                                 PAGE_FLAG_WRITE | PAGE_FLAG_USER);
        memset(page, 0, kPageSize);
    }

    cursors->next_stack = base;
    return Stack{base, top, total};
}

void release_user_region(uint64_t cr3, const Region& region) {
    if (region.base == 0 || region.length == 0) {
        return;
    }
    uint64_t base = align_down(region.base, kPageSize);
    uint64_t limit = align_up(region.length, kPageSize);
    for (uint64_t offset = 0; offset < limit; offset += kPageSize) {
        uint64_t virt = base + offset;
        uint64_t phys = 0;
        if (!paging_unmap_page_in_space(cr3, virt, phys)) {
            continue;
        }
        paging_free_physical(phys);
    }
}

bool copy_into_address_space(uint64_t cr3, uint64_t dest_user,
                             const void* src, size_t length) {
    if (length == 0) {
        return true;
    }
    if (src == nullptr) {
        return false;
    }
    const auto* src_bytes = static_cast<const uint8_t*>(src);
    size_t offset = 0;
    while (offset < length) {
        uint64_t virt = dest_user + offset;
        uint64_t phys = 0;
        if (!paging_translate(cr3, virt, phys)) {
            return false;
        }
        uint64_t page_offset = phys & kPageMask;
        size_t chunk = length - offset;
        size_t available = static_cast<size_t>(kPageSize - page_offset);
        if (chunk > available) {
            chunk = available;
        }
        uint64_t base = phys & ~kPageMask;
        uint64_t virt_base = paging_phys_to_virt(base);
        auto* dest_ptr =
            reinterpret_cast<uint8_t*>(virt_base + page_offset);
        memcpy(dest_ptr, src_bytes + offset, chunk);
        offset += chunk;
    }
    return true;
}

bool copy_from_address_space(uint64_t cr3, void* dest, uint64_t src_user,
                             size_t length) {
    if (length == 0) {
        return true;
    }
    if (dest == nullptr) {
        return false;
    }
    auto* dest_bytes = static_cast<uint8_t*>(dest);
    size_t offset = 0;
    while (offset < length) {
        uint64_t virt = src_user + offset;
        uint64_t phys = 0;
        if (!paging_translate(cr3, virt, phys)) {
            return false;
        }
        uint64_t page_offset = phys & kPageMask;
        size_t chunk = length - offset;
        size_t available = static_cast<size_t>(kPageSize - page_offset);
        if (chunk > available) {
            chunk = available;
        }
        uint64_t base = phys & ~kPageMask;
        uint64_t virt_base = paging_phys_to_virt(base);
        auto* src_ptr =
            reinterpret_cast<uint8_t*>(virt_base + page_offset);
        memcpy(dest_bytes + offset, src_ptr, chunk);
        offset += chunk;
    }
    return true;
}

bool zero_address_space(uint64_t cr3, uint64_t dest_user, size_t length) {
    if (length == 0) {
        return true;
    }
    size_t offset = 0;
    while (offset < length) {
        uint64_t virt = dest_user + offset;
        uint64_t phys = 0;
        if (!paging_translate(cr3, virt, phys)) {
            return false;
        }
        uint64_t page_offset = phys & kPageMask;
        size_t chunk = length - offset;
        size_t available = static_cast<size_t>(kPageSize - page_offset);
        if (chunk > available) {
            chunk = available;
        }
        uint64_t base = phys & ~kPageMask;
        uint64_t virt_base = paging_phys_to_virt(base);
        auto* dest_ptr =
            reinterpret_cast<uint8_t*>(virt_base + page_offset);
        memset(dest_ptr, 0, chunk);
        offset += chunk;
    }
    return true;
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
