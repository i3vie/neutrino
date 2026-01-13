#include "vm.hpp"

#include "arch/x86_64/memory/paging.hpp"
#include "kernel/memory/physical_allocator.hpp"
#include "lib/mem.hpp"
#include "drivers/log/logging.hpp"

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

Region map_user_code(uint64_t cr3,
                     const uint8_t* data,
                     size_t length,
                     uint64_t entry_offset, uint64_t& entry_point) {
    Region region{0, 0};
    if (cr3 == 0 || data == nullptr || length == 0) {
        entry_point = 0;
        return region;
    }

    uint64_t base = align_up(g_next_user_code, kPageSize);
    size_t padded = static_cast<size_t>(align_up(length, kPageSize));
    size_t pages = padded / kPageSize;

    for (size_t i = 0; i < pages; ++i) {
        uint64_t phys = memory::alloc_user_page();
        if (phys == 0) {
            entry_point = 0;
            return Region{0, 0};
        }
        auto* page = static_cast<uint8_t*>(paging_phys_to_virt(phys));
        uint64_t virt = base + static_cast<uint64_t>(i) * kPageSize;
        paging_map_page_cr3(cr3,
                            virt,
                            phys,
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
    g_next_user_code = region.base + region.length;

    uint64_t safe_offset = (entry_offset < length) ? entry_offset : 0;
    entry_point = region.base + safe_offset;
    return region;
}

Region reserve_user_region(size_t length) {
    Region region{0, 0};
    if (length == 0) {
        return region;
    }

    uint64_t base = align_up(g_next_user_code, kPageSize);
    size_t padded = static_cast<size_t>(align_up(length, kPageSize));
    size_t pages = padded / kPageSize;
    uint64_t total = static_cast<uint64_t>(pages) * kPageSize;
    if (!is_user_range(base, total)) {
        return Region{0, 0};
    }

    region.base = base;
    region.length = pages * kPageSize;
    g_next_user_code = region.base + region.length;
    return region;
}

Region allocate_user_region(uint64_t cr3, size_t length) {
    Region region{0, 0};
    if (cr3 == 0 || length == 0) {
        return region;
    }

    region = reserve_user_region(length);
    if (region.base == 0 || region.length == 0) {
        return Region{0, 0};
    }
    uint64_t base = region.base;
    size_t pages = region.length / kPageSize;

    for (size_t i = 0; i < pages; ++i) {
        uint64_t phys = memory::alloc_user_page();
        if (phys == 0) {
            return Region{0, 0};
        }
        auto* page = static_cast<uint8_t*>(paging_phys_to_virt(phys));
        memset(page, 0, kPageSize);
        uint64_t virt = base + static_cast<uint64_t>(i) * kPageSize;
        paging_map_page_cr3(cr3,
                            virt,
                            phys,
                            PAGE_FLAG_WRITE | PAGE_FLAG_USER);
    }

    return region;
}

Stack allocate_user_stack(uint64_t cr3, size_t length) {
    if (cr3 == 0) {
        log_message(LogLevel::Error,
                    "VM: stack alloc failed (cr3=0)");
        return Stack{0, 0, 0};
    }
    if (length == 0) {
        length = kPageSize;
    }
    size_t total = static_cast<size_t>(align_up(length, kPageSize));
    size_t pages = total / kPageSize;

    uint64_t top = align_down(g_next_user_stack, kPageSize);
    uint64_t base = top - static_cast<uint64_t>(total);

    for (size_t i = 0; i < pages; ++i) {
        uint64_t phys = memory::alloc_user_page();
        if (phys == 0) {
            log_message(LogLevel::Error,
                        "VM: stack alloc failed (page %zu/%zu)",
                        i + 1,
                        pages);
            return Stack{0, 0, 0};
        }
        auto* page = static_cast<uint8_t*>(paging_phys_to_virt(phys));
        uint64_t virt =
            base + static_cast<uint64_t>(i) * kPageSize;
        paging_map_page_cr3(cr3,
                            virt,
                            phys,
                            PAGE_FLAG_WRITE | PAGE_FLAG_USER);
        memset(page, 0, kPageSize);
    }

    g_next_user_stack = base;
    return Stack{base, top, total};
}

uint64_t map_region(uint64_t cr3,
                    uint64_t base,
                    size_t length,
                    uint64_t flags) {
    if (cr3 == 0 || base == 0 || length == 0) {
        return 0;
    }
    if ((base & kPageMask) != 0) {
        return 0;
    }
    size_t total = static_cast<size_t>(align_up(length, kPageSize));
    if (!is_user_range(base, total)) {
        return 0;
    }

    uint64_t map_flags = PAGE_FLAG_USER;
    if ((flags & kMapWrite) != 0) {
        map_flags |= PAGE_FLAG_WRITE;
    }

    for (uint64_t offset = 0; offset < total; offset += kPageSize) {
        uint64_t phys = 0;
        if (paging_resolve_cr3(cr3, base + offset, phys)) {
            return 0;
        }
    }

    for (uint64_t offset = 0; offset < total; offset += kPageSize) {
        uint64_t phys = memory::alloc_user_page();
        if (phys == 0) {
            for (uint64_t rollback = 0; rollback < offset; rollback += kPageSize) {
                uint64_t freed = 0;
                if (paging_unmap_page_cr3(cr3, base + rollback, freed)) {
                    memory::free_user_page(freed);
                }
            }
            return 0;
        }
        auto* page = static_cast<uint8_t*>(paging_phys_to_virt(phys));
        memset(page, 0, kPageSize);
        if (!paging_map_page_cr3(cr3, base + offset, phys, map_flags)) {
            memory::free_user_page(phys);
            for (uint64_t rollback = 0; rollback < offset; rollback += kPageSize) {
                uint64_t freed = 0;
                if (paging_unmap_page_cr3(cr3, base + rollback, freed)) {
                    memory::free_user_page(freed);
                }
            }
            return 0;
        }
    }

    return base;
}

uint64_t map_anonymous(uint64_t cr3, size_t length, uint64_t flags) {
    Region region = reserve_user_region(length);
    if (region.base == 0 || region.length == 0) {
        return 0;
    }
    return map_region(cr3, region.base, region.length, flags);
}

uint64_t map_at(uint64_t cr3, uint64_t addr_hint, size_t length, uint64_t flags) {
    if (addr_hint == 0) {
        return map_anonymous(cr3, length, flags);
    }
    return map_region(cr3, addr_hint, length, flags);
}

bool unmap_region(uint64_t cr3, uint64_t addr, size_t length) {
    if (cr3 == 0 || addr == 0 || length == 0) {
        return false;
    }
    if ((addr & kPageMask) != 0) {
        return false;
    }
    size_t total = static_cast<size_t>(align_up(length, kPageSize));
    if (!is_user_range(addr, total)) {
        return false;
    }

    for (uint64_t offset = 0; offset < total; offset += kPageSize) {
        uint64_t phys = 0;
        if (!paging_resolve_cr3(cr3, addr + offset, phys)) {
            return false;
        }
    }

    for (uint64_t offset = 0; offset < total; offset += kPageSize) {
        uint64_t phys = 0;
        if (paging_unmap_page_cr3(cr3, addr + offset, phys)) {
            memory::free_user_page(phys);
        }
    }
    return true;
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
        if (!paging_unmap_page_cr3(cr3, virt, phys)) {
            continue;
        }
        memory::free_user_page(phys);
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

bool copy_to_user(uint64_t cr3,
                  uint64_t dest,
                  const void* src,
                  size_t length) {
    if (length == 0) {
        return true;
    }
    if (cr3 == 0 || src == nullptr || dest == 0) {
        return false;
    }
    if (!is_user_range(dest, static_cast<uint64_t>(length))) {
        return false;
    }
    const auto* src_bytes = reinterpret_cast<const uint8_t*>(src);
    size_t offset = 0;
    while (offset < length) {
        uint64_t dest_addr = dest + offset;
        uint64_t phys = 0;
        if (!paging_resolve_cr3(cr3, dest_addr, phys)) {
            return false;
        }
        size_t page_off = static_cast<size_t>(dest_addr & kPageMask);
        size_t chunk = kPageSize - page_off;
        if (chunk > length - offset) {
            chunk = length - offset;
        }
        void* dest_ptr = paging_phys_to_virt(phys);
        memcpy(dest_ptr, src_bytes + offset, chunk);
        offset += chunk;
    }
    return true;
}

bool copy_from_user(uint64_t cr3,
                    void* dest,
                    uint64_t src,
                    size_t length) {
    if (length == 0) {
        return true;
    }
    if (cr3 == 0 || dest == nullptr || src == 0) {
        return false;
    }
    if (!is_user_range(src, static_cast<uint64_t>(length))) {
        return false;
    }
    auto* dest_bytes = reinterpret_cast<uint8_t*>(dest);
    size_t offset = 0;
    while (offset < length) {
        uint64_t src_addr = src + offset;
        uint64_t phys = 0;
        if (!paging_resolve_cr3(cr3, src_addr, phys)) {
            return false;
        }
        size_t page_off = static_cast<size_t>(src_addr & kPageMask);
        size_t chunk = kPageSize - page_off;
        if (chunk > length - offset) {
            chunk = length - offset;
        }
        void* src_ptr = paging_phys_to_virt(phys);
        memcpy(dest_bytes + offset, src_ptr, chunk);
        offset += chunk;
    }
    return true;
}

bool fill_user(uint64_t cr3,
               uint64_t dest,
               uint8_t value,
               size_t length) {
    if (length == 0) {
        return true;
    }
    if (cr3 == 0 || dest == 0) {
        return false;
    }
    if (!is_user_range(dest, static_cast<uint64_t>(length))) {
        return false;
    }
    size_t offset = 0;
    while (offset < length) {
        uint64_t dest_addr = dest + offset;
        uint64_t phys = 0;
        if (!paging_resolve_cr3(cr3, dest_addr, phys)) {
            return false;
        }
        size_t page_off = static_cast<size_t>(dest_addr & kPageMask);
        size_t chunk = kPageSize - page_off;
        if (chunk > length - offset) {
            chunk = length - offset;
        }
        void* dest_ptr = paging_phys_to_virt(phys);
        memset(dest_ptr, value, chunk);
        offset += chunk;
    }
    return true;
}

}  // namespace vm
