#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../drivers/console/console.hpp"
#include "../drivers/limine/limine_requests.hpp"
#include "../drivers/log/logging.hpp"
#include "arch/x86_64/gdt.hpp"
#include "arch/x86_64/idt.hpp"
#include "arch/x86_64/memory/paging.hpp"
#include "arch/x86_64/tss.hpp"
#include "lib/mem.hpp"

static void hcf(void) {
    for (;;) asm("hlt");
}

constexpr size_t BOOTSTRAP_STACK_SIZE = 0x8000;
alignas(16) static uint8_t bootstrap_stack[BOOTSTRAP_STACK_SIZE];

static void kernel_main_stage2();

extern "C" void kernel_main(void) {
    uint8_t* stack_top = bootstrap_stack + BOOTSTRAP_STACK_SIZE;
    asm volatile(
        "mov %0, %%rsp\n"
        "xor %%rbp, %%rbp\n"
        :
        : "r"(stack_top)
        : "memory");
    kernel_main_stage2();
}

static void kernel_main_stage2() {
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

    log_init();
    log_message(LogLevel::Info, "Console online");

    log_message(LogLevel::Info, "Welcome to Neutrino");

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

    log_message(LogLevel::Info, "Compiler: %s", compiler_string);

    log_message(LogLevel::Info, "Installing IDT");
    idt_install();
    log_message(LogLevel::Info, "IDT installed");

    log_message(LogLevel::Info, "Initializing TSS");
    init_tss();
    log_message(LogLevel::Info, "TSS initialized");

    log_message(LogLevel::Info, "Installing GDT");
    gdt_install();
    log_message(LogLevel::Info, "GDT installed");

    log_message(LogLevel::Debug, "Kernel phys base addr: %016x",
                (unsigned long long)kernel_addr_request.response->physical_base);
    log_message(LogLevel::Debug, "Kernel virt base addr: %016x",
                (unsigned long long)kernel_addr_request.response->virtual_base);
    log_message(LogLevel::Debug, "Kernel size: %u KB (%x)",
                (unsigned int)(kernel_file_request.response->kernel_file->size /
                               1024),
                (unsigned int)kernel_file_request.response->kernel_file->size);

    log_message(LogLevel::Debug, "HHDM offset: %016x",
                (unsigned long long)hhdm_request.response->offset);

    log_message(LogLevel::Info, "Initializing paging");
    paging_init();
    log_message(LogLevel::Info, "Paging initialized");

    hcf();
}
