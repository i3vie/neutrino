#include "idt.hpp"
#include <stdint.h>
#include "lib/mem.hpp"

struct __attribute__((packed)) IdtEntry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t zero;
};

struct __attribute__((packed)) IdtPtr {
    uint16_t limit;
    uint64_t base;
};

static IdtEntry idt[256];
static IdtPtr idt_ptr;

static void set_idt_entry(int vec, void* handler, uint8_t ist, uint8_t flags) {
    uintptr_t addr = (uintptr_t)handler;
    idt[vec].offset_low  = (uint16_t)(addr & 0xFFFF);
    idt[vec].selector    = 0x08;
    idt[vec].ist         = ist & 0x7;
    idt[vec].type_attr   = flags;
    idt[vec].offset_mid  = (uint16_t)((addr >> 16) & 0xFFFF);
    idt[vec].offset_high = (uint32_t)((addr >> 32) & 0xFFFFFFFF);
    idt[vec].zero        = 0;
}

extern void* isr_stub_table[256];

extern "C" void idt_install() {
    memset(idt, 0, sizeof(idt));
    for (int i = 0; i < 256; i++) {
        set_idt_entry(i, isr_stub_table[i], 0, 0x8E);
    }
    idt_ptr.limit = sizeof(idt) - 1;
    idt_ptr.base  = (uint64_t)&idt;
    asm volatile("lidt %0" :: "m"(idt_ptr));
}
