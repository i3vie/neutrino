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
constexpr uint64_t PAGE_HUGE_SIZE = 0x40000000;
constexpr uint64_t PAGE_HUGE_MASK = PAGE_HUGE_SIZE - 1;

constexpr uint64_t PTE_PRESENT = 1ull << 0;
constexpr uint64_t PTE_WRITE = 1ull << 1;
constexpr uint64_t PTE_USER = 1ull << 2;
constexpr uint64_t PTE_PWT = 1ull << 3;
constexpr uint64_t PTE_PCD = 1ull << 4;
constexpr uint64_t PTE_PAT = 1ull << 7;
constexpr uint64_t PTE_LARGE = 1ull << 7;
constexpr uint64_t PTE_GLOBAL = 1ull << 8;
constexpr uint64_t PTE_NX = 1ull << 63;

constexpr size_t PAGE_TABLE_ENTRIES = 512;

constexpr size_t BOOT_POOL_PAGES = 4096;
constexpr size_t BOOT_POOL_SIZE = BOOT_POOL_PAGES * PAGE_SIZE;

constexpr uint64_t LAPIC_BASE = 0xFEE00000;

alignas(PAGE_SIZE) uint8_t boot_pool[BOOT_POOL_SIZE];
size_t boot_pool_off = 0;
void* boot_pool_freelist[BOOT_POOL_PAGES];
size_t boot_pool_free_count = 0;

uint64_t g_kernel_phys_base = 0;
uint64_t g_kernel_virt_base = 0;
uint64_t g_kernel_size = 0;
uint64_t g_hhdm_offset = 0;
uint64_t g_cr3_value = 0;
uint64_t g_kernel_cr3 = 0;

uint64_t* pml4_table = nullptr;
constexpr size_t USER_PML4_LIMIT = 256;

uint64_t virt_to_phys(uint64_t virt);
uint64_t phys_to_virt(uint64_t phys);

uint64_t* pml4_from_cr3(uint64_t cr3) {
    if (cr3 == 0) {
        return nullptr;
    }
    uint64_t phys = cr3 & ~PAGE_MASK;
    return reinterpret_cast<uint64_t*>(phys_to_virt(phys));
}

void initialize_address_space(uint64_t* root) {
    if (root == nullptr) {
        return;
    }
    memcpy(root, pml4_table, PAGE_SIZE);
    for (size_t i = 0; i < USER_PML4_LIMIT; ++i) {
        if ((root[i] & PTE_USER) != 0) {
            root[i] = 0;
        }
    }
}

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
    if (boot_pool_free_count > 0) {
        void* v = boot_pool_freelist[--boot_pool_free_count];
        memset(v, 0, PAGE_SIZE);
        return v;
    }
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

void free_boot_page(void* page) {
    if (page == nullptr) {
        return;
    }
    auto* ptr = reinterpret_cast<uint8_t*>(page);
    auto* pool_begin = &boot_pool[0];
    auto* pool_end = pool_begin + BOOT_POOL_SIZE;
    if (ptr < pool_begin || ptr >= pool_end) {
        return;
    }
    if (boot_pool_free_count >= BOOT_POOL_PAGES) {
        return;
    }
    boot_pool_freelist[boot_pool_free_count++] = page;
}

uint64_t virt_to_phys(uint64_t virt) {
    uint64_t kernel_virt_end = g_kernel_virt_base + g_kernel_size;
    if (g_kernel_size != 0 && virt >= g_kernel_virt_base &&
        virt < kernel_virt_end) {
        return virt - g_kernel_virt_base + g_kernel_phys_base;
    }
    if (g_hhdm_offset != 0 && virt >= g_hhdm_offset) {
        return virt - g_hhdm_offset;
    }
    return virt;
}

uint64_t phys_to_virt(uint64_t phys) {
    uint64_t kernel_phys_end = g_kernel_phys_base + g_kernel_size;
    if (g_kernel_size != 0 && phys >= g_kernel_phys_base &&
        phys < kernel_phys_end) {
        return phys - g_kernel_phys_base + g_kernel_virt_base;
    }
    if (g_hhdm_offset != 0) {
        return phys + g_hhdm_offset;
    }
    return phys;
}

uint64_t* table_from_entry(uint64_t entry) {
    uint64_t phys = entry & ~PAGE_MASK;
    return reinterpret_cast<uint64_t*>(phys_to_virt(phys));
}

bool locate_page_entry(uint64_t* root, uint64_t virt, uint64_t*& pt_out,
                       size_t& pt_index_out) {
    if (root == nullptr) {
        return false;
    }

    size_t pml4_index = (virt >> 39) & 0x1FF;
    size_t pdpt_index = (virt >> 30) & 0x1FF;
    size_t pd_index = (virt >> 21) & 0x1FF;
    size_t pt_index = (virt >> 12) & 0x1FF;

    uint64_t pml4_entry = root[pml4_index];
    if ((pml4_entry & PTE_PRESENT) == 0) {
        return false;
    }
    uint64_t* pdpt = table_from_entry(pml4_entry);

    uint64_t pdpt_entry = pdpt[pdpt_index];
    if ((pdpt_entry & PTE_PRESENT) == 0) {
        return false;
    }
    uint64_t* pd = table_from_entry(pdpt_entry);

    uint64_t pd_entry = pd[pd_index];
    if ((pd_entry & PTE_PRESENT) == 0) {
        return false;
    }
    if ((pd_entry & PTE_LARGE) != 0) {
        return false;
    }

    uint64_t* pt = table_from_entry(pd_entry);
    pt_out = pt;
    pt_index_out = pt_index;
    return true;
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
        auto* child = static_cast<uint64_t*>(alloc_boot_page());
        uint64_t phys_base = entry & ~PAGE_LARGE_MASK;
        uint64_t base_flags = entry & ((1ull << 12) - 1);
        base_flags &= ~PTE_LARGE;
        base_flags |= PTE_PRESENT;
        uint64_t nx_flag = entry & PTE_NX;

        for (size_t i = 0; i < PAGE_TABLE_ENTRIES; ++i) {
            uint64_t child_phys = phys_base + (i * PAGE_SIZE);
            uint64_t child_entry = (child_phys & ~PAGE_MASK) | base_flags;
            child_entry |= nx_flag;
            child[i] = child_entry;
        }

        uint64_t child_phys_addr =
            virt_to_phys(reinterpret_cast<uint64_t>(child));
        uint64_t pointer_flags = entry & ((1ull << 12) - 1);
        pointer_flags &= ~PTE_LARGE;
        pointer_flags |= PTE_PRESENT | PTE_WRITE;
        table[index] = child_phys_addr | pointer_flags;
        entry = table[index];
    }

    if ((flags & PTE_USER) != 0 && (entry & PTE_USER) == 0) {
        table[index] |= PTE_USER;
        entry = table[index];
    }

    uint64_t phys = entry & ~PAGE_MASK;
    return reinterpret_cast<uint64_t*>(phys_to_virt(phys));
}

void map_page(uint64_t* root, uint64_t virt, uint64_t phys, uint64_t flags) {
    size_t pml4_index = (virt >> 39) & 0x1FF;
    size_t pdpt_index = (virt >> 30) & 0x1FF;
    size_t pd_index = (virt >> 21) & 0x1FF;
    size_t pt_index = (virt >> 12) & 0x1FF;

    uint64_t* pdpt = ensure_table(root, pml4_index, flags);
    uint64_t* pd = ensure_table(pdpt, pdpt_index, flags);
    uint64_t* pt = ensure_table(pd, pd_index, flags);

    pt[pt_index] = (phys & ~PAGE_MASK) | flags | PTE_PRESENT;
}

bool try_map_large_page(uint64_t* root, uint64_t virt, uint64_t phys,
                        uint64_t flags) {
    if ((virt & PAGE_LARGE_MASK) != 0 || (phys & PAGE_LARGE_MASK) != 0) {
        return false;
    }

    size_t pml4_index = (virt >> 39) & 0x1FF;
    size_t pdpt_index = (virt >> 30) & 0x1FF;
    size_t pd_index = (virt >> 21) & 0x1FF;

    uint64_t* pdpt = ensure_table(root, pml4_index, flags);
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

void map_range(uint64_t* root, uint64_t virt_start, uint64_t phys_start,
               uint64_t length, uint64_t flags) {
    if (length == 0) return;

    uint64_t phys_begin = align_down(phys_start, PAGE_SIZE);
    uint64_t phys_end = align_up(phys_start + length, PAGE_SIZE);

    uint64_t virt_begin = align_down(virt_start, PAGE_SIZE);
    uint64_t offset = virt_begin - phys_begin;

    for (uint64_t phys = phys_begin; phys < phys_end;) {
        uint64_t virt = phys + offset;
        if ((phys_end - phys) >= PAGE_LARGE_SIZE &&
            try_map_large_page(root, virt, phys, flags)) {
            phys += PAGE_LARGE_SIZE;
            continue;
        }

        map_page(root, virt, phys, flags);
        phys += PAGE_SIZE;
    }
}

void map_identity_range(uint64_t* root, uint64_t phys_start, uint64_t length,
                        uint64_t flags) {
    if (length == 0) return;

    uint64_t start = align_down(phys_start, PAGE_SIZE);
    uint64_t end = align_up(phys_start + length, PAGE_SIZE);

    for (uint64_t phys = start; phys < end;) {
        if ((end - phys) >= PAGE_LARGE_SIZE &&
            try_map_large_page(root, phys, phys, flags)) {
            phys += PAGE_LARGE_SIZE;
            continue;
        }

        map_page(root, phys, phys, flags);
        phys += PAGE_SIZE;
    }
}

bool should_map(uint64_t type) {
    switch (type) {
        case LIMINE_MEMMAP_USABLE:
        case LIMINE_MEMMAP_RESERVED:
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

        map_identity_range(pml4_table, entry->base, entry->length, map_flags);
        if (g_hhdm_offset != 0) {
            map_range(pml4_table, entry->base + g_hhdm_offset, entry->base,
                      entry->length, map_flags);
        }
    }

    if (g_kernel_size != 0) {
        map_range(pml4_table, g_kernel_virt_base, g_kernel_phys_base,
                  g_kernel_size, map_flags);
    }

    // Map Local APIC MMIO (identity + HHDM) with cache disabled.
    const uint64_t lapic_flags = PTE_PRESENT | PTE_WRITE | PTE_PCD | PTE_PWT;
    map_page(pml4_table, LAPIC_BASE, LAPIC_BASE, lapic_flags);
    if (g_hhdm_offset != 0) {
        map_page(pml4_table, LAPIC_BASE + g_hhdm_offset, LAPIC_BASE,
                 lapic_flags);
    }

    uint64_t new_cr3 = virt_to_phys(reinterpret_cast<uint64_t>(pml4_table));
    asm volatile("mov %0, %%cr3" : : "r"(new_cr3) : "memory");
    g_cr3_value = new_cr3;
    g_kernel_cr3 = new_cr3;

}

bool paging_map_page(uint64_t virt, uint64_t phys, uint64_t flags) {
    map_page(pml4_table, virt, phys, flags);
    asm volatile("invlpg (%0)" : : "r"(reinterpret_cast<void*>(virt)) : "memory");
    return true;
}

bool paging_map_page_in_space(uint64_t cr3, uint64_t virt, uint64_t phys,
                              uint64_t flags) {
    uint64_t* root = pml4_from_cr3(cr3);
    if (root == nullptr) {
        return false;
    }
    map_page(root, virt, phys, flags);
    return true;
}

uint64_t paging_virt_to_phys(uint64_t virt) {
    return virt_to_phys(virt);
}

uint64_t paging_phys_to_virt(uint64_t phys) {
    return phys_to_virt(phys);
}

void* paging_alloc_page() {
    return alloc_boot_page();
}

bool paging_unmap_page(uint64_t virt, uint64_t& phys_out) {
    phys_out = 0;
    uint64_t* pt = nullptr;
    size_t pt_index = 0;
    if (!locate_page_entry(pml4_table, virt, pt, pt_index)) {
        return false;
    }
    uint64_t entry = pt[pt_index];
    if ((entry & PTE_PRESENT) == 0) {
        return false;
    }
    phys_out = entry & ~PAGE_MASK;
    pt[pt_index] = 0;
    asm volatile("invlpg (%0)" : : "r"(reinterpret_cast<void*>(virt)) : "memory");
    return true;
}

bool paging_unmap_page_in_space(uint64_t cr3, uint64_t virt,
                                uint64_t& phys_out) {
    phys_out = 0;
    uint64_t* root = pml4_from_cr3(cr3);
    if (root == nullptr) {
        return false;
    }
    uint64_t* pt = nullptr;
    size_t pt_index = 0;
    if (!locate_page_entry(root, virt, pt, pt_index)) {
        return false;
    }
    uint64_t entry = pt[pt_index];
    if ((entry & PTE_PRESENT) == 0) {
        return false;
    }
    phys_out = entry & ~PAGE_MASK;
    pt[pt_index] = 0;
    return true;
}

bool paging_translate(uint64_t cr3, uint64_t virt, uint64_t& phys_out) {
    phys_out = 0;
    uint64_t* root = pml4_from_cr3(cr3);
    if (root == nullptr) {
        return false;
    }
    uint64_t* pt = nullptr;
    size_t pt_index = 0;
    if (!locate_page_entry(root, virt, pt, pt_index)) {
        return false;
    }
    uint64_t entry = pt[pt_index];
    if ((entry & PTE_PRESENT) == 0) {
        return false;
    }
    uint64_t page_offset = virt & PAGE_MASK;
    phys_out = (entry & ~PAGE_MASK) | page_offset;
    return true;
}

void paging_free_physical(uint64_t phys) {
    if (phys == 0) {
        return;
    }
    void* page = reinterpret_cast<void*>(phys_to_virt(phys));
    free_boot_page(page);
}

uint64_t paging_create_address_space() {
    auto* new_root = static_cast<uint64_t*>(alloc_boot_page());
    if (new_root == nullptr) {
        return 0;
    }
    initialize_address_space(new_root);
    uint64_t phys =
        virt_to_phys(reinterpret_cast<uint64_t>(new_root));
    return phys;
}

void paging_reset_address_space(uint64_t cr3) {
    uint64_t* root = pml4_from_cr3(cr3);
    initialize_address_space(root);
}

bool paging_mark_wc(uint64_t virt, uint64_t length) {
    if (length == 0) {
        return false;
    }

    uint64_t start = align_down(virt, PAGE_SIZE);
    uint64_t end = align_up(virt + length, PAGE_SIZE);

    for (uint64_t addr = start; addr < end; addr += PAGE_SIZE) {
        size_t pml4_index = (addr >> 39) & 0x1FF;
        size_t pdpt_index = (addr >> 30) & 0x1FF;
        size_t pd_index = (addr >> 21) & 0x1FF;
        size_t pt_index = (addr >> 12) & 0x1FF;

        uint64_t* pdpt = ensure_table(pml4_table, pml4_index, PTE_WRITE);
        uint64_t* pd = ensure_table(pdpt, pdpt_index, PTE_WRITE);
        uint64_t* pt = ensure_table(pd, pd_index, PTE_WRITE);

        uint64_t entry = pt[pt_index];
        if ((entry & PTE_PRESENT) == 0) {
            continue;
        }

        entry &= ~(PTE_PWT | PTE_PCD);
        entry |= PTE_PAT;
        pt[pt_index] = entry;

        asm volatile("invlpg (%0)" : : "r"(reinterpret_cast<void*>(addr)) : "memory");
    }

    return true;
}

uint64_t paging_cr3() {
    return g_cr3_value;
}

uint64_t paging_kernel_cr3() {
    return g_kernel_cr3;
}

uint64_t paging_hhdm_offset() {
    return g_hhdm_offset;
}

uint64_t paging_kernel_phys_base() {
    return g_kernel_phys_base;
}

uint64_t paging_kernel_phys_size() {
    return g_kernel_size;
}

void paging_switch_cr3(uint64_t new_cr3) {
    if (new_cr3 == 0 || new_cr3 == g_cr3_value) {
        return;
    }
    asm volatile("mov %0, %%cr3" : : "r"(new_cr3) : "memory");
    g_cr3_value = new_cr3;
}

uint64_t paging_create_address_space() {
    if (pml4_table == nullptr) {
        return 0;
    }
    auto* new_root = static_cast<uint64_t*>(alloc_boot_page());
    if (new_root == nullptr) {
        return 0;
    }
    memset(new_root, 0, PAGE_SIZE);
    constexpr size_t kKernelStart = PAGE_TABLE_ENTRIES / 2;
    for (size_t i = kKernelStart; i < PAGE_TABLE_ENTRIES; ++i) {
        new_root[i] = pml4_table[i];
    }
    return virt_to_phys(reinterpret_cast<uint64_t>(new_root));
}

bool paging_map_page_cr3(uint64_t cr3, uint64_t virt, uint64_t phys, uint64_t flags) {
    if (cr3 == 0) {
        return false;
    }
    uint64_t root_phys = cr3 & ~PAGE_MASK;
    auto* root = reinterpret_cast<uint64_t*>(phys_to_virt(root_phys));
    map_page_with_root(root, virt, phys, flags);
    asm volatile("invlpg (%0)" : : "r"(reinterpret_cast<void*>(virt)) : "memory");
    return true;
}

bool paging_unmap_page_cr3(uint64_t cr3, uint64_t virt, uint64_t& phys_out) {
    phys_out = 0;
    if (cr3 == 0) {
        return false;
    }
    uint64_t root_phys = cr3 & ~PAGE_MASK;
    auto* root = reinterpret_cast<uint64_t*>(phys_to_virt(root_phys));
    size_t pml4_index = (virt >> 39) & 0x1FF;
    size_t pdpt_index = (virt >> 30) & 0x1FF;
    size_t pd_index = (virt >> 21) & 0x1FF;
    size_t pt_index = (virt >> 12) & 0x1FF;

    uint64_t pml4_entry = root[pml4_index];
    if ((pml4_entry & PTE_PRESENT) == 0) {
        return false;
    }
    uint64_t* pdpt = table_from_entry(pml4_entry);

    uint64_t pdpt_entry = pdpt[pdpt_index];
    if ((pdpt_entry & PTE_PRESENT) == 0) {
        return false;
    }
    if ((pdpt_entry & PTE_LARGE) != 0) {
        return false;
    }
    uint64_t* pd = table_from_entry(pdpt_entry);

    uint64_t pd_entry = pd[pd_index];
    if ((pd_entry & PTE_PRESENT) == 0 || (pd_entry & PTE_LARGE) != 0) {
        return false;
    }
    uint64_t* pt = table_from_entry(pd_entry);

    uint64_t pt_entry = pt[pt_index];
    if ((pt_entry & PTE_PRESENT) == 0) {
        return false;
    }
    phys_out = pt_entry & ~PAGE_MASK;
    pt[pt_index] = 0;
    asm volatile("invlpg (%0)" : : "r"(reinterpret_cast<void*>(virt)) : "memory");
    return true;
}

bool paging_resolve_cr3(uint64_t cr3, uint64_t virt, uint64_t& phys_out) {
    phys_out = 0;
    if (cr3 == 0) {
        return false;
    }
    uint64_t root_phys = cr3 & ~PAGE_MASK;
    auto* root = reinterpret_cast<uint64_t*>(phys_to_virt(root_phys));
    size_t pml4_index = (virt >> 39) & 0x1FF;
    size_t pdpt_index = (virt >> 30) & 0x1FF;
    size_t pd_index = (virt >> 21) & 0x1FF;
    size_t pt_index = (virt >> 12) & 0x1FF;

    uint64_t pml4_entry = root[pml4_index];
    if ((pml4_entry & PTE_PRESENT) == 0) {
        return false;
    }
    uint64_t* pdpt = table_from_entry(pml4_entry);

    uint64_t pdpt_entry = pdpt[pdpt_index];
    if ((pdpt_entry & PTE_PRESENT) == 0) {
        return false;
    }
    if ((pdpt_entry & PTE_LARGE) != 0) {
        uint64_t base = pdpt_entry & ~PAGE_HUGE_MASK;
        phys_out = base + (virt & PAGE_HUGE_MASK);
        return true;
    }
    uint64_t* pd = table_from_entry(pdpt_entry);

    uint64_t pd_entry = pd[pd_index];
    if ((pd_entry & PTE_PRESENT) == 0) {
        return false;
    }
    if ((pd_entry & PTE_LARGE) != 0) {
        uint64_t base = pd_entry & ~PAGE_LARGE_MASK;
        phys_out = base + (virt & PAGE_LARGE_MASK);
        return true;
    }
    uint64_t* pt = table_from_entry(pd_entry);

    uint64_t pt_entry = pt[pt_index];
    if ((pt_entry & PTE_PRESENT) == 0) {
        return false;
    }
    phys_out = (pt_entry & ~PAGE_MASK) | (virt & PAGE_MASK);
    return true;
}

void* paging_phys_to_virt(uint64_t phys) {
    if (g_hhdm_offset != 0) {
        return reinterpret_cast<void*>(phys + g_hhdm_offset);
    }
    return reinterpret_cast<void*>(phys_to_virt(phys));
}
