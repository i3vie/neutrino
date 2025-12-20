#pragma once
#include <stdint.h>

constexpr uint64_t PAGE_FLAG_WRITE  = 1ull << 1;
constexpr uint64_t PAGE_FLAG_USER   = 1ull << 2;
constexpr uint64_t PAGE_FLAG_GLOBAL = 1ull << 8;

void paging_init();
bool paging_map_page(uint64_t virt, uint64_t phys, uint64_t flags);
bool paging_map_page_in_space(uint64_t cr3, uint64_t virt, uint64_t phys,
                              uint64_t flags);
uint64_t paging_virt_to_phys(uint64_t virt);
uint64_t paging_phys_to_virt(uint64_t phys);
void* paging_alloc_page();
bool paging_mark_wc(uint64_t virt, uint64_t length);
uint64_t paging_cr3();
bool paging_unmap_page(uint64_t virt, uint64_t& phys_out);
bool paging_unmap_page_in_space(uint64_t cr3, uint64_t virt,
                                uint64_t& phys_out);
bool paging_translate(uint64_t cr3, uint64_t virt, uint64_t& phys_out);
void paging_free_physical(uint64_t phys);
uint64_t paging_create_address_space();
void paging_reset_address_space(uint64_t cr3);
