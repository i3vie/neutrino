#pragma once

#include <stddef.h>
#include <stdint.h>

#ifndef LIMINE_API_REVISION
#define LIMINE_API_REVISION 3
#endif

#include "./limine.h"

struct PreservedLimineModule {
    const void* address;
    uint64_t size;
    const char* path;
    const char* string;
};

extern volatile struct limine_framebuffer_request framebuffer_request;
extern volatile struct limine_hhdm_request hhdm_request;
#if LIMINE_API_REVISION >= 2
extern volatile struct limine_executable_file_request kernel_file_request;
#else
extern volatile struct limine_kernel_file_request kernel_file_request;
#endif
#if LIMINE_API_REVISION >= 2
extern volatile struct limine_executable_address_request kernel_addr_request;
#else
extern volatile struct limine_kernel_address_request kernel_addr_request;
#endif
extern volatile struct limine_memmap_request memmap_request;
extern volatile struct limine_executable_cmdline_request cmdline_request;
extern volatile struct limine_module_request module_request;
extern volatile struct LIMINE_MP(request) smp_request;
extern volatile struct limine_rsdp_request rsdp_request;

void preserve_limine_modules();
size_t preserved_limine_module_count();
const PreservedLimineModule* preserved_limine_module(size_t index);
