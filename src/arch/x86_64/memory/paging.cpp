#include "paging.hpp"
#include "lib/mem.hpp"
#include "../../drivers/console/console.hpp"
#include <stdint.h>

extern Console* kconsole;

static uint64_t g_kernel_phys_base = 0;
static uint64_t g_kernel_virt_base = 0;
static uint64_t g_kernel_size      = 0;

static uint8_t boot_pool[0x20000] __attribute__((aligned(0x1000)));
static size_t boot_pool_off = 0;

static void* alloc_boot_page() {
    if (boot_pool_off >= sizeof(boot_pool))
        return nullptr;
    void* v = &boot_pool[boot_pool_off];
    boot_pool_off += 0x1000;
    memset(v, 0, 0x1000);
    return v;
}

uint64_t virt_to_phys(uint64_t virt) {
    return virt - g_kernel_virt_base + g_kernel_phys_base;
}

static inline void invlpg(void* addr) {
    asm volatile("invlpg (%0)" :: "r"(addr) : "memory");
}

void map_page(uint64_t virt, uint64_t phys, uint64_t flags) {
    kconsole->printf("[paging] STUB map_page: %016x -> %016x (flags=%016x)\n",
                     virt, phys, flags);
    invlpg((void*)virt);
}

void paging_init(uint64_t kernel_phys_base,
                 uint64_t kernel_virt_base,
                 uint64_t kernel_size)
{
    g_kernel_phys_base = kernel_phys_base;
    g_kernel_virt_base = kernel_virt_base;
    g_kernel_size      = kernel_size;

    kconsole->printf("[paging] init: phys=%016x virt=%016x size=%016x\n",
                     g_kernel_phys_base, g_kernel_virt_base, g_kernel_size);

    uint64_t* pml4        = (uint64_t*)alloc_boot_page();
    uint64_t* pdpt_low    = (uint64_t*)alloc_boot_page();
    uint64_t* pd_low      = (uint64_t*)alloc_boot_page();
    uint64_t* pdpt_kernel = (uint64_t*)alloc_boot_page();

    for (uint64_t addr = 0; addr < 0x40000000ull; addr += 0x200000ull)
        pd_low[(addr >> 21) & 0x1FF] = addr | PAGE_PRESENT | PAGE_WRITE | PAGE_HUGE;

    pdpt_low[0] = virt_to_phys((uint64_t)pd_low) | PAGE_PRESENT | PAGE_WRITE;
    pml4[0]     = virt_to_phys((uint64_t)pdpt_low) | PAGE_PRESENT | PAGE_WRITE;

    const uint64_t KWIN_SIZE  = 256ull * 1024 * 1024;
    const uint64_t v_start    = g_kernel_virt_base & ~0xFFFull;
    const uint64_t v_end      = v_start + KWIN_SIZE;
    const int64_t  delta      = (int64_t)g_kernel_phys_base - (int64_t)g_kernel_virt_base;

    const size_t pml4e_kernel = (v_start >> 39) & 0x1FF;
    pml4[pml4e_kernel] = virt_to_phys((uint64_t)pdpt_kernel) | PAGE_PRESENT | PAGE_WRITE;

    uint64_t* pd_for_pdpte[512] = {0};
    uint64_t* pt_for_pd[512][512] = {{0}};

    for (uint64_t v = v_start; v < v_end; v += 0x1000) {
        uint64_t p = (uint64_t)((int64_t)v + delta);
        size_t pdpte = (v >> 30) & 0x1FF;
        size_t pde   = (v >> 21) & 0x1FF;
        size_t pte   = (v >> 12) & 0x1FF;

        if (!pd_for_pdpte[pdpte]) {
            pd_for_pdpte[pdpte] = (uint64_t*)alloc_boot_page();
            pdpt_kernel[pdpte] = virt_to_phys((uint64_t)pd_for_pdpte[pdpte]) | PAGE_PRESENT | PAGE_WRITE;
        }
        uint64_t* pd = pd_for_pdpte[pdpte];

        if (!pt_for_pd[pdpte][pde]) {
            pt_for_pd[pdpte][pde] = (uint64_t*)alloc_boot_page();
            pd[pde] = virt_to_phys((uint64_t)pt_for_pd[pdpte][pde]) | PAGE_PRESENT | PAGE_WRITE;
        }
        uint64_t* pt = pt_for_pd[pdpte][pde];

        pt[pte] = p | PAGE_PRESENT | PAGE_WRITE;
    }

    uint64_t new_cr3 = virt_to_phys((uint64_t)pml4);
    asm volatile("mov %0, %%cr3" :: "r"(new_cr3) : "memory");

    kconsole->printf("[paging] switched. CR3=%016x PML4[0]=%016x PML4[%zu]=%016x\n",
                     new_cr3, pml4[0], (size_t)pml4e_kernel, pml4[pml4e_kernel]);
}

void map_range(uint64_t virt, uint64_t phys, size_t size, uint64_t flags) {
    size_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    for (size_t i = 0; i < pages; i++)
        map_page(virt + i * PAGE_SIZE, phys + i * PAGE_SIZE, flags);
}
