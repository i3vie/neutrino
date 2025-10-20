#pragma once
#include <stdint.h>
#include <stddef.h>

constexpr uint64_t PAGE_PRESENT  = 1ull << 0;   // page present in memory
constexpr uint64_t PAGE_WRITE    = 1ull << 1;   // writable
constexpr uint64_t PAGE_USER     = 1ull << 2;   // user-mode accessible
constexpr uint64_t PAGE_PWT      = 1ull << 3;   // page write-through
constexpr uint64_t PAGE_PCD      = 1ull << 4;   // page cache disable
constexpr uint64_t PAGE_ACCESSED = 1ull << 5;   // accessed (set by CPU)
constexpr uint64_t PAGE_DIRTY    = 1ull << 6;   // dirty (set by CPU, only in PT)
constexpr uint64_t PAGE_HUGE     = 1ull << 7;   // 1 GiB or 2 MiB page
constexpr uint64_t PAGE_GLOBAL   = 1ull << 8;   // global mapping (TBL not flushed)
constexpr uint64_t PAGE_NX       = 1ull << 63;  // no-execute (requires NX bit in EFER)

constexpr uint64_t PAGE_SIZE = 0x1000;          // 4 KiB pages

void paging_init(uint64_t kernel_phys_base,
                 uint64_t kernel_virt_base,
                 uint64_t kernel_size);

void map_page(uint64_t virt, uint64_t phys, uint64_t flags);
void map_range(uint64_t virt, uint64_t phys, size_t size, uint64_t flags);
