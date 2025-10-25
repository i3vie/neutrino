#include "arch/x86_64/memory/paging.hpp"

#include <stddef.h>
#include <stdint.h>

#include "../../drivers/console/console.hpp"
#include "drivers/limine/limine_requests.hpp"
#include "lib/mem.hpp"

extern "C" char kernel_start[];
extern "C" char kernel_end[];

namespace {

constexpr uint64_t PAGE_SIZE = 0x1000;
constexpr uint64_t PAGE_MASK = PAGE_SIZE - 1;
constexpr uint64_t PAGE_LARGE_SIZE = 0x200000;
constexpr uint64_t PAGE_LARGE_MASK = PAGE_LARGE_SIZE - 1;

constexpr uint64_t PTE_PRESENT = 1ull << 0;
constexpr uint64_t PTE_WRITE = 1ull << 1;
constexpr uint64_t PTE_USER = 1ull << 2;
constexpr uint64_t PTE_LARGE = 1ull << 7;
constexpr uint64_t PTE_GLOBAL = 1ull << 8;

constexpr size_t BOOT_POOL_PAGES = 4096;
constexpr size_t BOOT_POOL_SIZE = BOOT_POOL_PAGES * PAGE_SIZE;

alignas(PAGE_SIZE) uint8_t boot_pool[BOOT_POOL_SIZE];
size_t boot_pool_off = 0;

uint64_t g_kernel_phys_base = 0;
uint64_t g_kernel_virt_base = 0;
uint64_t g_kernel_size = 0;
uint64_t g_hhdm_offset = 0;

uint64_t* pml4_table = nullptr;

constexpr uint64_t align_down(uint64_t value, uint64_t alignment) {
    return value & ~(alignment - 1);
}

constexpr uint64_t align_up(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

[[noreturn]] void halt_system() {
    asm volatile("cli; hlt");
    for (;;) {
        asm volatile("hlt");
    }
}

void* alloc_boot_page() {
    if (boot_pool_off + PAGE_SIZE > BOOT_POOL_SIZE) {
        if (kconsole != nullptr) {
            kconsole->set_color(0xFFFF0000, 0x00000000);
            kconsole->printf("Paging bootstrap pool exhausted, halting.\n");
        }
        halt_system();
    }

    void* v = &boot_pool[boot_pool_off];
    boot_pool_off += PAGE_SIZE;
    memset(v, 0, PAGE_SIZE);
    return v;
}

uint64_t virt_to_phys(uint64_t virt) {
    return virt - g_kernel_virt_base + g_kernel_phys_base;
}

uint64_t phys_to_virt(uint64_t phys) {
    return phys - g_kernel_phys_base + g_kernel_virt_base;
}

uint64_t* ensure_table(uint64_t* table, size_t index, uint64_t flags) {
    uint64_t entry = table[index];
    if ((entry & PTE_PRESENT) == 0) {
        auto child = static_cast<uint64_t*>(alloc_boot_page());
        uint64_t phys = virt_to_phys(reinterpret_cast<uint64_t>(child));
        uint64_t child_flags = PTE_PRESENT | PTE_WRITE;
        if (flags & PTE_USER) {
            child_flags |= PTE_USER;
        }
        table[index] = phys | child_flags;
        return child;
    }

    if (entry & PTE_LARGE) {
        // We should never treat a large page mapping as a pointer to a table.
        // Jesus fucking christ I'm gonna do it this time
        if (kconsole != nullptr) {
            kconsole->set_color(0xFFFF0000, 0x00000000);
            kconsole->printf("Attempted to treat large page entry as table.\n");
        }
        halt_system();
    }

    if ((flags & PTE_USER) != 0 && (entry & PTE_USER) == 0) {
        table[index] |= PTE_USER;
        entry = table[index];
    }

    uint64_t phys = entry & ~PAGE_MASK;
    return reinterpret_cast<uint64_t*>(phys_to_virt(phys));
}

void map_page(uint64_t virt, uint64_t phys, uint64_t flags) {
    size_t pml4_index = (virt >> 39) & 0x1FF;
    size_t pdpt_index = (virt >> 30) & 0x1FF;
    size_t pd_index = (virt >> 21) & 0x1FF;
    size_t pt_index = (virt >> 12) & 0x1FF;

    uint64_t* pdpt = ensure_table(pml4_table, pml4_index, flags);
    uint64_t* pd = ensure_table(pdpt, pdpt_index, flags);
    uint64_t* pt = ensure_table(pd, pd_index, flags);

    pt[pt_index] = (phys & ~PAGE_MASK) | flags | PTE_PRESENT;
}

bool try_map_large_page(uint64_t virt, uint64_t phys, uint64_t flags) {
    if ((virt & PAGE_LARGE_MASK) != 0 || (phys & PAGE_LARGE_MASK) != 0) {
        return false;
    }

    size_t pml4_index = (virt >> 39) & 0x1FF;
    size_t pdpt_index = (virt >> 30) & 0x1FF;
    size_t pd_index = (virt >> 21) & 0x1FF;

    uint64_t* pdpt = ensure_table(pml4_table, pml4_index, flags);
    uint64_t* pd = ensure_table(pdpt, pdpt_index, flags);

    uint64_t entry = pd[pd_index];
    if (entry & PTE_PRESENT) {
        if ((entry & PTE_LARGE) != 0) {
            return true;  // Already mapped as large page
        }
        return false;  // Already mapped via 4K pages, cannot convert
    }

    pd[pd_index] = (phys & ~PAGE_LARGE_MASK) | flags | PTE_PRESENT | PTE_LARGE;
    return true;
}

void map_range(uint64_t virt_start, uint64_t phys_start, uint64_t length,
               uint64_t flags) {
    if (length == 0) return;

    uint64_t phys_begin = align_down(phys_start, PAGE_SIZE);
    uint64_t phys_end = align_up(phys_start + length, PAGE_SIZE);

    uint64_t virt_begin = align_down(virt_start, PAGE_SIZE);
    uint64_t offset = virt_begin - phys_begin;

    for (uint64_t phys = phys_begin; phys < phys_end;) {
        uint64_t virt = phys + offset;
        if ((phys_end - phys) >= PAGE_LARGE_SIZE &&
            try_map_large_page(virt, phys, flags)) {
            phys += PAGE_LARGE_SIZE;
            continue;
        }

        map_page(virt, phys, flags);
        phys += PAGE_SIZE;
    }
}

void map_identity_range(uint64_t phys_start, uint64_t length, uint64_t flags) {
    if (length == 0) return;

    uint64_t start = align_down(phys_start, PAGE_SIZE);
    uint64_t end = align_up(phys_start + length, PAGE_SIZE);

    for (uint64_t phys = start; phys < end;) {
        if ((end - phys) >= PAGE_LARGE_SIZE &&
            try_map_large_page(phys, phys, flags)) {
            phys += PAGE_LARGE_SIZE;
            continue;
        }

        map_page(phys, phys, flags);
        phys += PAGE_SIZE;
    }
}

bool should_map(uint64_t type) {
    switch (type) {
        case LIMINE_MEMMAP_USABLE:
        case LIMINE_MEMMAP_ACPI_RECLAIMABLE:
        case LIMINE_MEMMAP_ACPI_NVS:
        case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE:
#if LIMINE_API_REVISION >= 2
        case LIMINE_MEMMAP_EXECUTABLE_AND_MODULES:
#else
        case LIMINE_MEMMAP_KERNEL_AND_MODULES:
#endif
        case LIMINE_MEMMAP_FRAMEBUFFER:
            return true;
        default:
            return false;
    }
}

}  // namespace

void paging_init() {
    if (memmap_request.response == nullptr ||
        kernel_addr_request.response == nullptr) {
        halt_system();
    }
    if (hhdm_request.response != nullptr) {
        g_hhdm_offset = hhdm_request.response->offset;
    } else {
        g_hhdm_offset = 0;
    }

    g_kernel_phys_base = kernel_addr_request.response->physical_base;
    g_kernel_virt_base = reinterpret_cast<uint64_t>(&kernel_start);
    uint64_t kernel_virtual_end = reinterpret_cast<uint64_t>(&kernel_end);
    if (kernel_virtual_end < g_kernel_virt_base) {
        halt_system();
    }
    g_kernel_size =
        align_up(kernel_virtual_end - g_kernel_virt_base, PAGE_SIZE);

    boot_pool_off = 0;
    pml4_table = static_cast<uint64_t*>(alloc_boot_page());

    const uint64_t map_flags = PTE_WRITE | PTE_GLOBAL;

    auto memmap = memmap_request.response;
    for (uint64_t i = 0; i < memmap->entry_count; ++i) {
        auto entry = memmap->entries[i];
        if (entry == nullptr || entry->length == 0) {
            continue;
        }

        if (!should_map(entry->type)) {
            continue;
        }

        map_identity_range(entry->base, entry->length, map_flags);
        if (g_hhdm_offset != 0) {
            map_range(entry->base + g_hhdm_offset, entry->base, entry->length,
                      map_flags);
        }
    }

    if (g_kernel_size != 0) {
        map_range(g_kernel_virt_base, g_kernel_phys_base, g_kernel_size,
                  map_flags);
    }

    uint64_t new_cr3 = virt_to_phys(reinterpret_cast<uint64_t>(pml4_table));
    asm volatile("mov %0, %%cr3" : : "r"(new_cr3) : "memory");
}

bool paging_map_page(uint64_t virt, uint64_t phys, uint64_t flags) {
    map_page(virt, phys, flags);
    asm volatile("invlpg (%0)" : : "r"(reinterpret_cast<void*>(virt)) : "memory");
    return true;
}

uint64_t paging_virt_to_phys(uint64_t virt) {
    return virt_to_phys(virt);
}

void* paging_alloc_page() {
    return alloc_boot_page();
}
