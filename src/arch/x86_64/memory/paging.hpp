#pragma once
#include <stdint.h>
#include <stddef.h>

constexpr uint64_t PAGE_PRESENT  = 1ull << 0;
constexpr uint64_t PAGE_WRITE    = 1ull << 1;   
constexpr uint64_t PAGE_USER     = 1ull << 2;
constexpr uint64_t PAGE_PWT      = 1ull << 3; 
constexpr uint64_t PAGE_PCD      = 1ull << 4; 
constexpr uint64_t PAGE_ACCESSED = 1ull << 5;
constexpr uint64_t PAGE_DIRTY    = 1ull << 6;
constexpr uint64_t PAGE_HUGE     = 1ull << 7;
constexpr uint64_t PAGE_GLOBAL   = 1ull << 8;
constexpr uint64_t PAGE_NX       = 1ull << 63;

constexpr uint64_t PAGE_SIZE = 0x1000;

void paging_init(uint64_t kernel_phys_base,
                 uint64_t kernel_virt_base,
                 uint64_t kernel_size);

void map_page(uint64_t virt, uint64_t phys, uint64_t flags);
void map_range(uint64_t virt, uint64_t phys, size_t size, uint64_t flags);

uint64_t virt_to_phys(uint64_t virt);
uint64_t phys_to_virt(uint64_t phys);
