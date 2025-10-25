#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../drivers/console/console.hpp"
#include "../drivers/limine/limine_requests.hpp"
#include "../drivers/log/logging.hpp"
#include "../drivers/fs/fat32/fat32.hpp"
#include "../drivers/fs/mount_manager.hpp"
#include "../drivers/interrupts/pic.hpp"
#include "../drivers/timer/pit.hpp"
#include "../fs/vfs.hpp"
#include "loader.hpp"
#include "process.hpp"
#include "scheduler.hpp"
#include "arch/x86_64/gdt.hpp"
#include "arch/x86_64/idt.hpp"
#include "arch/x86_64/memory/paging.hpp"
#include "arch/x86_64/syscall.hpp"
#include "arch/x86_64/tss.hpp"

static void hcf(void) {
    for (;;) asm("hlt");
}

namespace {

size_t string_length(const char* str) {
    size_t len = 0;
    if (str == nullptr) {
        return 0;
    }
    while (str[len] != '\0') {
        ++len;
    }
    return len;
}

void copy_string(char* dest, size_t dest_size, const char* src) {
    if (dest == nullptr || dest_size == 0) {
        return;
    }
    size_t idx = 0;
    while (idx + 1 < dest_size && src[idx] != '\0') {
        dest[idx] = src[idx];
        ++idx;
    }
    dest[idx] = '\0';
}

bool starts_with(const char* str, const char* prefix) {
    if (str == nullptr || prefix == nullptr) {
        return false;
    }
    while (*prefix) {
        if (*str++ != *prefix++) {
            return false;
        }
    }
    return true;
}

bool strings_equal(const char* a, const char* b) {
    if (a == nullptr || b == nullptr) {
        return false;
    }
    while (*a && *b) {
        if (*a != *b) {
            return false;
        }
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

}  // namespace

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

    log_message(LogLevel::Info, "Initializing syscall interface");
    syscall::init();
    log_message(LogLevel::Info, "Syscall interface initialized");

    log_message(LogLevel::Info, "Initializing PIC");
    pic::init();
    log_message(LogLevel::Info, "PIC initialized");

    log_message(LogLevel::Info, "Configuring PIT");
    pit::init(100);
    log_message(LogLevel::Info, "PIT configured");

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

    vfs::init();

    const char* cmdline = nullptr;
    if (cmdline_request.response != nullptr &&
        cmdline_request.response->cmdline != nullptr) {
        cmdline = cmdline_request.response->cmdline;
    }

    constexpr size_t kMaxCmdline = 256;
    constexpr size_t kMaxMountSpecs = 16;
    constexpr size_t kMaxSpecLen = 32;

    char root_spec[kMaxSpecLen] = {0};
    char mount_buffers[kMaxMountSpecs][kMaxSpecLen] = {{0}};
    const char* mount_specs[kMaxMountSpecs];
    for (size_t i = 0; i < kMaxMountSpecs; ++i) {
        mount_specs[i] = nullptr;
    }
    size_t mount_spec_count = 0;

    if (cmdline != nullptr) {
        char buffer[kMaxCmdline];
        size_t len = string_length(cmdline);
        if (len >= sizeof(buffer)) {
            len = sizeof(buffer) - 1;
        }
        for (size_t i = 0; i < len; ++i) {
            buffer[i] = cmdline[i];
        }
        buffer[len] = '\0';

        char* ptr = buffer;
        while (*ptr != '\0') {
            while (*ptr == ' ') {
                ++ptr;
            }
            if (*ptr == '\0') {
                break;
            }
            char* token = ptr;
            while (*ptr != '\0' && *ptr != ' ') {
                ++ptr;
            }
            if (*ptr == ' ') {
                *ptr++ = '\0';
            }

            if (starts_with(token, "ROOT=")) {
                const char* value = token + 5;
                if (*value != '\0') {
                    copy_string(root_spec, sizeof(root_spec), value);
                }
            } else if (starts_with(token, "MOUNT=")) {
                const char* value = token + 6;
                if (*value == '\0') {
                    continue;
                }
                bool duplicate = (root_spec[0] != '\0' &&
                                   strings_equal(root_spec, value));
                for (size_t i = 0; i < mount_spec_count && !duplicate; ++i) {
                    if (strings_equal(mount_buffers[i], value)) {
                        duplicate = true;
                    }
                }
                if (duplicate) {
                    continue;
                }
                if (mount_spec_count < kMaxMountSpecs) {
                    copy_string(mount_buffers[mount_spec_count],
                                sizeof(mount_buffers[mount_spec_count]),
                                value);
                    mount_specs[mount_spec_count] =
                        mount_buffers[mount_spec_count];
                    ++mount_spec_count;
                } else {
                    log_message(LogLevel::Warn,
                                "Boot: ignoring extra MOUNT=%s (limit %zu)",
                                value,
                                static_cast<size_t>(kMaxMountSpecs));
                }
            }
        }
    }

    if (root_spec[0] == '\0') {
        log_message(LogLevel::Warn,
                    "boot: ROOT= not specified on kernel command line");
    } else {
        log_message(LogLevel::Info,
                    "boot: ROOT=%s", root_spec); 
    }

    size_t mounted_count = 0;
    const char* root_ptr = (root_spec[0] != '\0') ? root_spec : nullptr;
    bool root_ok = fs::mount_requested_filesystems(root_ptr, mount_specs,
                                                   mount_spec_count,
                                                   mounted_count);
    if (root_ptr != nullptr && !root_ok) {
        log_message(LogLevel::Warn,
                    "Boot: root filesystem '%s' was not mounted",
                    root_ptr);
    }

    constexpr size_t kMountQueryLimit = 16;
    size_t total_mounts = vfs::enumerate_mounts(nullptr, 0);
    size_t fetch_mounts =
        (total_mounts < kMountQueryLimit) ? total_mounts : kMountQueryLimit;

    const char* mount_names[kMountQueryLimit];
    if (fetch_mounts > 0) {
        vfs::enumerate_mounts(mount_names, fetch_mounts);
    }

    log_message(LogLevel::Info, "VFS: mounted filesystems: %u",
                static_cast<unsigned int>(mounted_count));
    log_message(LogLevel::Info, "VFS: available mounts: %u",
                static_cast<unsigned int>(total_mounts));
    for (size_t i = 0; i < fetch_mounts; ++i) {
        log_message(LogLevel::Info, "  %s/", mount_names[i]);
    }
    if (fetch_mounts < total_mounts) {
        log_message(LogLevel::Info,
                    "VFS: additional mounts not listed due to buffer size");
    }

    if (root_ptr != nullptr && root_ok) {
        Fat32DirEntry entries[32];
        size_t entry_count = 0;
        if (vfs::list(root_ptr, entries, 32, entry_count)) {
            log_message(LogLevel::Info,
                        "VFS: %s contains %u entries",
                        root_ptr,
                        static_cast<unsigned int>(entry_count));
        }

        char path[64];
        copy_string(path, sizeof(path), root_ptr);
        size_t base_len = string_length(path);
        if (base_len + 1 < sizeof(path)) {
            path[base_len] = '/';
            ++base_len;

            const char* target = "KERNEL.CFG";
            size_t target_len = string_length(target);
            if (base_len + target_len < sizeof(path)) {
                for (size_t idx = 0; idx < target_len; ++idx) {
                    path[base_len + idx] = target[idx];
                }
                path[base_len + target_len] = '\0';

                uint8_t file_buffer[1024];
                size_t file_size = 0;
                if (vfs::read_file(path, file_buffer, sizeof(file_buffer),
                                   file_size)) {
                    log_message(LogLevel::Info,
                                "VFS: read %s (%u bytes)",
                                path,
                                (unsigned int)file_size);
                } else {
                    log_message(LogLevel::Debug,
                                "VFS: %s not present or read failed",
                                path);
                }
            }
        }
    } else {
        log_message(LogLevel::Warn,
                    "Boot: skipping KERNEL.CFG lookup (root not mounted)");
    }

    process::init();
    scheduler::init();
    scheduler::run();

    log_message(LogLevel::Error,
                "Scheduler failed to start or was exited, halting");
    hcf();
}
