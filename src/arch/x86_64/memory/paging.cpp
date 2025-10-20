#include "paging.hpp"
#include "lib/mem.hpp"
#include "../../drivers/console/console.hpp"
#include <stdint.h>

extern Console* kconsole;

static uint64_t g_kernel_phys;
static uint64_t g_kernel_virt;

static uint8_t kernel_page_pool[0x20000] __attribute__((aligned(0x1000)));
static size_t pool_off = 0;

static void* alloc_kernel_page() {
    if (pool_off >= sizeof(kernel_page_pool))
        for (;;) asm("hlt");

    void* page = &kernel_page_pool[pool_off];
    pool_off += 0x1000;
    memset(page, 0, 0x1000);
    return page;
}

static inline uint64_t virt_to_phys(uint64_t virt) {
    return virt - g_kernel_virt + g_kernel_phys;
}

void paging_init(uint64_t kernel_phys_base,
                 uint64_t kernel_virt_base,
                 uint64_t kernel_size)
{
    g_kernel_phys = kernel_phys_base;
    g_kernel_virt = kernel_virt_base;

    kconsole->printf("[paging] kernel_phys=%016x kernel_virt=%016x\n",
                     g_kernel_phys, g_kernel_virt);

    uint64_t* new_pml4    = (uint64_t*)alloc_kernel_page();
    uint64_t* pdpt_low    = (uint64_t*)alloc_kernel_page();
    uint64_t* pd_low      = (uint64_t*)alloc_kernel_page();
    uint64_t* pdpt_kernel = (uint64_t*)alloc_kernel_page();
    uint64_t* pd_kernel   = (uint64_t*)alloc_kernel_page();

    for (uint64_t addr = 0; addr < 0x40000000; addr += 0x200000)
        pd_low[(addr >> 21) & 0x1FF] =
            addr | PAGE_PRESENT | PAGE_WRITE | PAGE_HUGE; 

    pdpt_low[0] = virt_to_phys((uint64_t)pd_low) | PAGE_PRESENT | PAGE_WRITE;
    new_pml4[0] = virt_to_phys((uint64_t)pdpt_low) | PAGE_PRESENT | PAGE_WRITE;

    // map kernel
    uint64_t kernel_pages = (kernel_size + 0x1FFFFF) >> 21;
    for (uint64_t i = 0; i < kernel_pages; i++) {
        uint64_t phys = kernel_phys_base + i * 0x200000;
        pd_kernel[i] = phys | PAGE_PRESENT | PAGE_WRITE | PAGE_HUGE; // just use huge pages for now
    }

    pdpt_kernel[0] = virt_to_phys((uint64_t)pd_kernel) | PAGE_PRESENT | PAGE_WRITE;
    new_pml4[511] = virt_to_phys((uint64_t)pdpt_kernel) | PAGE_PRESENT | PAGE_WRITE;

    // identity-map the new tables themselves so the CPU can read them
    auto id_map = [&](uint64_t phys) {
        uint64_t idx = (phys >> 21) & 0x1FF;
        pd_low[idx] = (phys & ~0x1FFFFFULL) | PAGE_PRESENT | PAGE_WRITE | PAGE_HUGE;
        kconsole->printf("[paging] id-mapped table phys=%016x in pd_low[%d]\n",
                         phys, (int)idx);
    };

    id_map(virt_to_phys((uint64_t)new_pml4));
    id_map(virt_to_phys((uint64_t)pdpt_low));
    id_map(virt_to_phys((uint64_t)pd_low));
    id_map(virt_to_phys((uint64_t)pdpt_kernel));
    id_map(virt_to_phys((uint64_t)pd_kernel));

    uint64_t new_cr3 = virt_to_phys((uint64_t)new_pml4);

    kconsole->printf("[paging] new_cr3 phys=%016x, switching...\n", new_cr3);
    kconsole->printf("[paging] mapping high-half %d pages from phys=%016x\n",
                     (int)kernel_pages, kernel_phys_base);
    for (size_t i = 0; i < kernel_pages; i++)
        kconsole->printf("  PD[%d] = %016x\n", (int)i, pd_kernel[i]);
    
        // Print the value of cr3
    
    uint64_t value;
    asm volatile("mov %%cr3, %0" : "=r"(value));

    kconsole->printf("CR3: %016x", value);

    asm volatile("mov %0, %%cr3" :: "r"(new_cr3) : "memory");
    kconsole->printf("[paging] switched successfully.\n");
}

void map_page(uint64_t virt, uint64_t phys, uint64_t flags) {
    kconsole->printf("[paging] map_page: not yet implemented for 4 KiB pages (%016x -> %016x)\n",
                     virt, phys);
}

void map_range(uint64_t virt, uint64_t phys, size_t size, uint64_t flags) {
    size_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    for (size_t i = 0; i < pages; i++)
        map_page(virt + i * PAGE_SIZE, phys + i * PAGE_SIZE, flags);
}
