#pragma once

#include <stddef.h>
#include <stdint.h>

namespace memory {

void init();
bool kernel_allocator_ready();

uint64_t alloc_kernel_block_pages(size_t pages);
uint64_t alloc_kernel_page();
void free_kernel_block(uint64_t phys);
void free_kernel_page(uint64_t phys);

uint64_t alloc_user_page();
void free_user_page(uint64_t phys);

uint64_t kernel_pool_base();
uint64_t kernel_pool_size();

}  // namespace memory
