#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "../drivers/limine/limine_requests.hpp"
#include "arch/x86_64/gdt.hpp"
#include "arch/x86_64/idt.hpp"

static void hcf(void) {
    for (;;) asm("hlt");
}

extern "C" void kernel_main(void) {
    if (framebuffer_request.response == nullptr
     || framebuffer_request.response->framebuffer_count == 0) {
        // limine didn't give us a framebuffer
        hcf();
    }

    // paint the whole screen blue
    auto fb = *framebuffer_request.response->framebuffers;
    for (uint64_t y = 0; y < fb->height; y++) {
        for (uint64_t x = 0; x < fb->width; x++) {
            uint32_t* pixel = (uint32_t*)((uintptr_t)fb->address + (y * fb->pitch) + (x * (fb->bpp / 8)));
            *pixel = 0x000000FF; // should be ARGB
        }
    }

    idt_install();
    gdt_install();

    hcf();
    
}
