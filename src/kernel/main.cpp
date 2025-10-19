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

    auto fb = *framebuffer_request.response->framebuffers;

    enum class RampState {
        R_UP,
        G_UP,
        B_UP,
        R_DOWN,
        G_DOWN,
        B_DOWN
    };

    uint8_t r = 0, g = 0, b = 0;
    RampState state = RampState::R_UP;
    uint8_t step = 1;

    for (;;) {
        switch (state) {
            case RampState::R_UP:   r += step; if (r >= 255) { r = 255; state = RampState::G_UP; } break;
            case RampState::G_UP:   g += step; if (g >= 255) { g = 255; state = RampState::B_UP; } break;
            case RampState::B_UP:   b += step; if (b >= 255) { b = 255; state = RampState::R_DOWN; } break;
            case RampState::R_DOWN: r -= step; if (r <= 0)   { r = 0;   state = RampState::G_DOWN; } break;
            case RampState::G_DOWN: g -= step; if (g <= 0)   { g = 0;   state = RampState::B_DOWN; } break;
            case RampState::B_DOWN: b -= step; if (b <= 0)   { b = 0;   state = RampState::R_UP; } break;
        }

        for (uint64_t y = 0; y < fb->height; y++) {
            for (uint64_t x = 0; x < fb->width; x++) {
                uint8_t gr = (x * 255 / fb->width);
                uint8_t gg = (y * 255 / fb->height);
                uint8_t gb = ((x * 255 / fb->width + y * 255 / fb->height) / 2);

                uint8_t final_r = (gr + r) / 2;
                uint8_t final_g = (gg + g) / 2;
                uint8_t final_b = (gb + b) / 2;

                uint32_t color = (0xFF << 24) | (final_r << 16) | (final_g << 8) | final_b;
                uint32_t* pixel = (uint32_t*)((uintptr_t)fb->address + (y * fb->pitch) + (x * 4));
                *pixel = color;
            }
        }
    }



    idt_install();
    gdt_install();

    hcf();
    
}
