#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../drivers/console/console.hpp"
#include "../drivers/limine/limine_requests.hpp"
#include "arch/x86_64/gdt.hpp"
#include "arch/x86_64/idt.hpp"
#include "arch/x86_64/memory/paging.hpp"
#include "arch/x86_64/tss.hpp"
#include "lib/mem.hpp"

static void hcf(void) {
    for (;;) asm("hlt");
}

extern "C" void kernel_main(void) {
    if (framebuffer_request.response == nullptr ||
        framebuffer_request.response->framebuffer_count == 0) {
        // limine didn't give us a framebuffer
        hcf();
    }

    auto fb = *framebuffer_request.response->framebuffers[0];
    Framebuffer framebuffer = {(uint8_t*)fb.address, fb.width, fb.height,
                               fb.pitch};
    Console console = Console(&framebuffer);
    kconsole = &console;  // HAVE AT THEE

    console.puts("Welcome to ");
    console.set_color(0xFFEBA134, 0x00000000);
    console.puts("Neutrino\n");
    console.set_color(0xFFFFFFFF, 0x00000000);
    const char* compiler_string =
#if defined(__clang__)
        "Clang/LLVM " __clang_version__;
#elif defined(__GNUC__) || defined(__GNUG__)
        "GCC " __VERSION__;
#elif defined(_MSC_VER)
        "MSVC" __MSC_FULL_VER_STR;
#else
        "Unknown compiler";
#endif

    asm volatile("cli");

    console.puts("Compiler: ");
    console.set_color(0xFFEBA134, 0x00000000);
    console.puts(compiler_string);
    console.putc('\n');
    console.set_color(0xFFFFFFFF, 0x00000000);

    console.puts("Installing IDT             ");
    idt_install();
    console.set_color(0xFF00B000, 0x00000000);
    console.puts("[OK]\n");
    console.set_color(0xFFFFFFFF, 0x00000000);

    console.puts("Initializing TSS           ");
    init_tss();
    console.set_color(0xFF00B000, 0x00000000);
    console.puts("[OK]\n");
    console.set_color(0xFFFFFFFF, 0x00000000);

    console.puts("Installing GDT             ");
    gdt_install();
    console.set_color(0xFF00B000, 0x00000000);
    console.puts("[OK]\n");
    console.set_color(0xFFFFFFFF, 0x00000000);

    uint64_t kernel_phys_base = (uint64_t)kernel_file_request.response->kernel_file->address;
    console.printf("Kernel phys base addr:     %x\n", kernel_phys_base);
    console.printf("Kernel size:               %d KB (%x)\n", kernel_file_request.response->kernel_file->size / 1024, kernel_file_request.response->kernel_file->size);

    console.puts("Initializing paging        ");
    paging_init(kernel_phys_base, 0xffffffff80000000ULL, kernel_file_request.response->kernel_file->size);
    console.set_color(0xFF00B000, 0x00000000);
    console.puts("[OK]\n");
    console.set_color(0xFFFFFFFF, 0x00000000);

    uint64_t test_phys = 0x00200000; // 2 mb
    uint64_t test_virt = 0xffffffff90000000;

    map_page(test_virt, test_phys, PAGE_PRESENT | PAGE_WRITE);

    hcf();
}
