#pragma once
#include <stdint.h>

constexpr uint64_t PAGE_FLAG_WRITE  = 1ull << 1;
constexpr uint64_t PAGE_FLAG_USER   = 1ull << 2;
constexpr uint64_t PAGE_FLAG_GLOBAL = 1ull << 8;

void paging_init();
bool paging_map_page(uint64_t virt, uint64_t phys, uint64_t flags);
uint64_t paging_virt_to_phys(uint64_t virt);
void* paging_alloc_page();
bool paging_mark_wc(uint64_t virt, uint64_t length);
uint64_t paging_cr3();
uint64_t paging_kernel_cr3();
uint64_t paging_hhdm_offset();
uint64_t paging_kernel_phys_base();
uint64_t paging_kernel_phys_size();
void paging_switch_cr3(uint64_t new_cr3);
uint64_t paging_create_address_space();
bool paging_map_page_cr3(uint64_t cr3, uint64_t virt, uint64_t phys, uint64_t flags);
bool paging_unmap_page_cr3(uint64_t cr3, uint64_t virt, uint64_t& phys_out);
bool paging_resolve_cr3(uint64_t cr3, uint64_t virt, uint64_t& phys_out);
void* paging_phys_to_virt(uint64_t phys);
bool paging_unmap_page(uint64_t virt, uint64_t& phys_out);
void paging_free_physical(uint64_t phys);
