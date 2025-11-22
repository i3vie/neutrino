#include "gdt.hpp"
#include "tss.hpp"
#include "lib/mem.hpp"
#include <stdint.h>

struct GdtEntry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  gran;
    uint8_t  base_high;
} __attribute__((packed));

alignas(16) static uint8_t gdt_area[(8 * 8)];

static void set_gdt_entry_bytes(uint8_t* dst, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran) {
    dst[0] = limit & 0xFF;
    dst[1] = (limit >> 8) & 0xFF;
    dst[2] = base & 0xFF;
    dst[3] = (base >> 8) & 0xFF;
    dst[4] = (base >> 16) & 0xFF;
    dst[5] = access;
    dst[6] = ((limit >> 16) & 0x0F) | (gran & 0xF0);
    dst[7] = (base >> 24) & 0xFF;
}

static void set_tss_descriptor_bytes(uint8_t* dst, uint64_t base, uint32_t limit, uint8_t access, uint8_t gran) {
    uint64_t low = 0;
    low  = (limit & 0xFFFF);
    low |= (base & 0xFFFFFF) << 16;
    low |= ((uint64_t)access) << 40;
    low |= ((limit >> 16) & 0xF) << 48; 
    low |= ((uint64_t)(gran & 0xF0)) << 52;
    low |= ((base >> 24) & 0xFF) << 56;

    // high 8 needs to be zeroed
    uint64_t high = (base >> 32) & 0xFFFFFFFF;
    high |= 0;

    *(uint64_t*)(dst + 0) = low;
    *(uint64_t*)(dst + 8) = high;
}

extern "C" void load_gdt_ptr(const void* ptr); // gdt_load.S

static void build_gdt(uint8_t* area, size_t area_size, TSS* tss_ptr) {
    if (area_size < sizeof(gdt_area)) {
        return;
    }
    memset(area, 0, sizeof(gdt_area));
    set_gdt_entry_bytes(area + 0 * 8, 0, 0, 0, 0); // null
    set_gdt_entry_bytes(area + 1 * 8, 0, 0x000FFFFF, 0x9A, 0x20); // kernel code (L=1)
    set_gdt_entry_bytes(area + 2 * 8, 0, 0x000FFFFF, 0x92, 0x00); // kernel data
    set_gdt_entry_bytes(area + 3 * 8, 0, 0x000FFFFF, 0xFA, 0x20); // user code DPL=3 L=1
    set_gdt_entry_bytes(area + 4 * 8, 0, 0x000FFFFF, 0xF2, 0x00); // user data DPL=3
    set_gdt_entry_bytes(area + 5 * 8, 0, 0x000FFFFF, 0x9A, 0x20); // fallback code (for stray selectors)
    set_tss_descriptor_bytes(area + 6 * 8, (uint64_t)tss_ptr, sizeof(TSS) - 1, 0x89, 0x00);
}

static void load_gdt(uint8_t* area) {
    struct {
        uint16_t limit;
        uint64_t base;
    } __attribute__((packed)) gdtr;

    gdtr.limit = (uint16_t)((8 * 8) - 1);
    gdtr.base  = (uint64_t)area;

    load_gdt_ptr(&gdtr);
}

void gdt_install_for_cpu(TSS* tss_ptr, uint8_t* gdt_storage, size_t storage_size) {
    if (tss_ptr == nullptr || gdt_storage == nullptr) {
        return;
    }
    build_gdt(gdt_storage, storage_size, tss_ptr);
    load_gdt(gdt_storage);
}

void gdt_install() {
    build_gdt(gdt_area, sizeof(gdt_area), &tss);
    load_gdt(gdt_area);
}
