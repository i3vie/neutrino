#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../drivers/console/console.hpp"
#include "../drivers/fs/mount_manager.hpp"
#include "../drivers/input/keyboard.hpp"
#include "../drivers/interrupts/pic.hpp"
#include "../drivers/limine/limine_requests.hpp"
#include "../drivers/log/logging.hpp"
#include "../drivers/pci/pci.hpp"
#include "../drivers/timer/pit.hpp"
#include "../fs/vfs.hpp"
#include "arch/x86_64/gdt.hpp"
#include "arch/x86_64/idt.hpp"
#include "arch/x86_64/memory/paging.hpp"
#include "arch/x86_64/pat.hpp"
#include "arch/x86_64/mtrr.hpp"
#include "arch/x86_64/syscall.hpp"
#include "arch/x86_64/tss.hpp"
#include "arch/x86_64/io.hpp"
#include "arch/x86_64/lapic.hpp"
#include "arch/x86_64/percpu.hpp"
#include "arch/x86_64/smp.hpp"
#include "config.hpp"
#include "descriptor.hpp"
#include "loader.hpp"
#include "process.hpp"
#include "scheduler.hpp"
#include "string_util.hpp"

static void hcf(void) {
    for (;;) asm("hlt");
}

namespace {

constexpr const char kRootDevicePlaceholder[] = "RootDevice";
bool matches_literal(const char* str, size_t len, const char* literal) {
    if (str == nullptr || literal == nullptr) {
        return false;
    }
    size_t idx = 0;
    while (idx < len && literal[idx] != '\0') {
        if (str[idx] != literal[idx]) {
            return false;
        }
        ++idx;
    }
    return (idx == len) && (literal[idx] == '\0');
}

bool append_chunk(const char* data,
                  size_t length,
                  char* out,
                  size_t out_size,
                  size_t& index) {
    if (data == nullptr || out == nullptr) {
        return false;
    }
    if (length == 0) {
        return true;
    }
    if (index + length >= out_size) {
        return false;
    }
    for (size_t i = 0; i < length; ++i) {
        out[index++] = data[i];
    }
    return true;
}

bool build_mount_path(const char* spec,
                      const char* default_mount,
                      char* out,
                      size_t out_size) {
    if (spec == nullptr || out == nullptr || out_size == 0) {
        return false;
    }

    if (spec[0] == '(') {
        const char* mount_start = spec + 1;
        const char* mount_end = mount_start;
        while (*mount_end != '\0' && *mount_end != ')') {
            ++mount_end;
        }
        if (*mount_end != ')') {
            return false;
        }
        size_t mount_len = static_cast<size_t>(mount_end - mount_start);
        if (mount_len == 0) {
            return false;
        }

        const char* remainder = mount_end + 1;
        while (*remainder == '/') {
            ++remainder;
        }
        size_t remainder_len = string_util::length(remainder);
        if (remainder_len == 0) {
            return false;
        }

        const char* mount_value = mount_start;
        size_t mount_value_len = mount_len;
        if (matches_literal(mount_start,
                            mount_len,
                            kRootDevicePlaceholder)) {
            if (default_mount == nullptr || default_mount[0] == '\0') {
                return false;
            }
            mount_value = default_mount;
            mount_value_len = string_util::length(default_mount);
        }

        size_t idx = 0;
        if (!append_chunk(mount_value,
                          mount_value_len,
                          out,
                          out_size,
                          idx)) {
            return false;
        }
        if (idx + 1 >= out_size) {
            return false;
        }
        out[idx++] = '/';
        if (!append_chunk(remainder,
                          remainder_len,
                          out,
                          out_size,
                          idx)) {
            return false;
        }
        if (idx >= out_size) {
            return false;
        }
        out[idx] = '\0';
        return true;
    }

    size_t spec_len = string_util::length(spec);
    if (spec_len == 0) {
        return false;
    }

    bool has_slash = string_util::contains(spec, '/');
    if (has_slash || default_mount == nullptr || default_mount[0] == '\0') {
        size_t idx = 0;
        if (!append_chunk(spec, spec_len, out, out_size, idx)) {
            return false;
        }
        if (idx >= out_size) {
            return false;
        }
        out[idx] = '\0';
        return true;
    }

    size_t mount_len = string_util::length(default_mount);
    if (mount_len == 0) {
        return false;
    }

    size_t idx = 0;
    if (!append_chunk(default_mount,
                      mount_len,
                      out,
                      out_size,
                      idx)) {
        return false;
    }
    if (idx + 1 >= out_size) {
        return false;
    }
    out[idx++] = '/';
    if (!append_chunk(spec,
                      spec_len,
                      out,
                      out_size,
                      idx)) {
        return false;
    }
    if (idx >= out_size) {
        return false;
    }
    out[idx] = '\0';
    return true;
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
    uint8_t* fb_virtual = static_cast<uint8_t*>(fb.address);
    uint64_t fb_phys_addr = reinterpret_cast<uint64_t>(fb.address);
    if (hhdm_request.response != nullptr &&
        hhdm_request.response->offset != 0 &&
        fb_phys_addr >= hhdm_request.response->offset) {
        fb_phys_addr -= hhdm_request.response->offset;
    }
    uint64_t fb_length =
        static_cast<uint64_t>(fb.pitch) * static_cast<uint64_t>(fb.height);

    Framebuffer framebuffer = {
        fb_virtual,
        static_cast<size_t>(fb.width),
        static_cast<size_t>(fb.height),
        static_cast<size_t>(fb.pitch),
        static_cast<uint16_t>(fb.bpp),
        fb.memory_model,
        fb.red_mask_size,
        fb.red_mask_shift,
        fb.green_mask_size,
        fb.green_mask_shift,
        fb.blue_mask_size,
        fb.blue_mask_shift};

    descriptor::init();
    descriptor::register_builtin_types();

    descriptor::register_framebuffer_device(framebuffer, fb_phys_addr);
    uint32_t framebuffer_handle =
        descriptor::open_kernel(descriptor::kTypeFramebuffer, 0, 0, 0);
    if (framebuffer_handle == descriptor::kInvalidHandle) {
        log_message(LogLevel::Warn,
                    "Console: failed to open framebuffer descriptor");
    }
    Console console = Console(framebuffer_handle);
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

    log_message(LogLevel::Info, "Initializing per-CPU state (BSP)");
    uint32_t bsp_lapic = lapic::id();
    percpu::Cpu* bsp_cpu = percpu::find_by_lapic(bsp_lapic);
    if (bsp_cpu == nullptr) {
        bsp_cpu = percpu::register_cpu(bsp_lapic, 0);
    }
    percpu::set_current_cpu(bsp_cpu);
    percpu::setup_cpu_tss(*bsp_cpu);
    percpu::setup_cpu_gdt(*bsp_cpu);
    scheduler::register_cpu(bsp_cpu);
    log_message(LogLevel::Info, "BSP per-CPU GDT/TSS installed (LAPIC=%u)",
                bsp_lapic);

    log_message(LogLevel::Info, "Initializing syscall interface");
    syscall::init();
    log_message(LogLevel::Info, "Syscall interface initialized");

    log_message(LogLevel::Info, "Initializing PIC");
    pic::init();
    log_message(LogLevel::Info, "PIC initialized");

    log_message(LogLevel::Info, "Initializing keyboard");
    keyboard::init();
    log_message(LogLevel::Info, "Keyboard initialized");

    log_message(LogLevel::Info, "Configuring PIT");
    pit::init(100);
    log_message(LogLevel::Info, "PIT configured");

    log_message(
        LogLevel::Debug, "Kernel phys base addr: %016x",
        (unsigned long long)kernel_addr_request.response->physical_base);
    log_message(LogLevel::Debug, "Kernel virt base addr: %016x",
                (unsigned long long)kernel_addr_request.response->virtual_base);
    log_message(
        LogLevel::Debug, "Kernel size: %u KB (%x)",
        (unsigned int)(kernel_file_request.response->kernel_file->size / 1024),
        (unsigned int)kernel_file_request.response->kernel_file->size);

    log_message(LogLevel::Debug, "HHDM offset: %016x",
                (unsigned long long)hhdm_request.response->offset);

    log_message(LogLevel::Info, "Initializing paging");
    paging_init();
    if (kconsole != nullptr) {
        kconsole->enable_back_buffer();
    }
    log_message(LogLevel::Info, "Paging initialized");

    lapic::init(hhdm_request.response->offset);
    log_message(LogLevel::Info, "Local APIC initialized");

    smp::init();

    bool pat_ok = cpu::configure_pat_write_combining();
    bool wc_pages = false;
    if (pat_ok && fb_virtual != nullptr) {
        wc_pages = paging_mark_wc(reinterpret_cast<uint64_t>(fb_virtual),
                                  fb_length);
    }
    bool mtrr_ok = cpu::configure_write_combining(fb_phys_addr, fb_length);

    if (!pat_ok) {
        log_message(LogLevel::Warn,
                    "PAT: failed to configure write-combining entry");
    } else if (!wc_pages) {
        log_message(LogLevel::Warn,
                    "PAT: failed to mark framebuffer pages WC (virt=%016llx len=%llu)",
                    reinterpret_cast<unsigned long long>(fb_virtual),
                    static_cast<unsigned long long>(fb_length));
    }

    if (!mtrr_ok) {
        log_message(LogLevel::Warn,
                    "Framebuffer WC configuration failed (phys=%016llx len=%llu)",
                    static_cast<unsigned long long>(fb_phys_addr),
                    static_cast<unsigned long long>(fb_length));
    }

    log_message(LogLevel::Info, "Initializing PCI subsystem");
    pci::init();
    log_message(LogLevel::Info, "PCI subsystem initialized");

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
        size_t len = string_util::length(cmdline);
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

            if (string_util::starts_with(token, "ROOT=")) {
                const char* value = token + 5;
                if (*value != '\0') {
                    string_util::copy(root_spec, sizeof(root_spec), value);
                }
            } else if (string_util::starts_with(token, "MOUNT=")) {
                const char* value = token + 6;
                if (*value == '\0') {
                    continue;
                }
                bool duplicate =
                    (root_spec[0] != '\0' &&
                     string_util::equals(root_spec, value));
                for (size_t i = 0; i < mount_spec_count && !duplicate; ++i) {
                    if (string_util::equals(mount_buffers[i], value)) {
                        duplicate = true;
                    }
                }
                if (duplicate) {
                    continue;
                }
                if (mount_spec_count < kMaxMountSpecs) {
                    string_util::copy(mount_buffers[mount_spec_count],
                                      sizeof(mount_buffers[mount_spec_count]),
                                      value);
                    mount_specs[mount_spec_count] =
                        mount_buffers[mount_spec_count];
                    ++mount_spec_count;
                } else {
                    log_message(LogLevel::Warn,
                                "Boot: ignoring extra MOUNT=%s (limit %zu)",
                                value, static_cast<size_t>(kMaxMountSpecs));
                }
            }
        }
    }

    if (root_spec[0] == '\0') {
        log_message(LogLevel::Warn,
                    "boot: ROOT= not specified on kernel command line");
    } else {
        log_message(LogLevel::Info, "boot: ROOT=%s", root_spec);
    }

    size_t mounted_count = 0;
    const char* root_ptr = (root_spec[0] != '\0') ? root_spec : nullptr;
    bool root_ok = fs::mount_requested_filesystems(
        root_ptr, mount_specs, mount_spec_count, mounted_count);
    if (root_ptr != nullptr && !root_ok) {
        log_message(LogLevel::Warn,
                    "Boot: root filesystem '%s' was not mounted", root_ptr);
    }

    char boot_mount_name[64] = {0};
    if (root_ptr != nullptr && root_ok) {
        string_util::copy(boot_mount_name, sizeof(boot_mount_name), root_ptr);
    }
    char boot_cwd[128];
    boot_cwd[0] = '/';
    boot_cwd[1] = '\0';

    config::Table kernel_config{};
    bool kernel_config_loaded = false;
    char init_task_path[64] = {0};
    bool init_task_path_valid = false;

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

    if (boot_mount_name[0] == '\0') {
        for (size_t i = 0; i < fetch_mounts; ++i) {
            if (mount_names[i] != nullptr) {
                string_util::copy(boot_mount_name,
                                  sizeof(boot_mount_name),
                                  mount_names[i]);
                break;
            }
        }
    }

    if (boot_mount_name[0] != '\0') {
        size_t idx = 1;
        size_t mount_len = string_util::length(boot_mount_name);
        if (mount_len + 1 >= sizeof(boot_cwd)) {
            mount_len = sizeof(boot_cwd) - 2;
        }
        for (size_t i = 0; i < mount_len; ++i) {
            boot_cwd[idx++] = boot_mount_name[i];
        }
        boot_cwd[idx] = '\0';
    } else {
        boot_cwd[1] = '\0';
    }

    if (root_ptr != nullptr && root_ok) {
        vfs::DirEntry entries[32];
        size_t entry_count = 0;
        if (vfs::list(root_ptr, entries, 32, entry_count)) {
            log_message(LogLevel::Info, "VFS: %s contains %u entries", root_ptr,
                        static_cast<unsigned int>(entry_count));
        }

        char path[64];
        string_util::copy(path, sizeof(path), root_ptr);
        size_t base_len = string_util::length(path);
        if (base_len + 1 < sizeof(path)) {
            path[base_len] = '/';
            ++base_len;

            const char* target = "KERNEL.CFG";
            size_t target_len = string_util::length(target);
            if (base_len + target_len < sizeof(path)) {
                for (size_t idx = 0; idx < target_len; ++idx) {
                    path[base_len + idx] = target[idx];
                }
                path[base_len + target_len] = '\0';

                uint8_t file_buffer[1024];
                size_t file_size = 0;
                if (vfs::read_file(path, file_buffer, sizeof(file_buffer),
                                   file_size)) {
                    log_message(LogLevel::Info, "VFS: read %s (%u bytes)", path,
                                (unsigned int)file_size);
                    bool parse_ok = config::parse(
                        reinterpret_cast<const char*>(file_buffer),
                        file_size,
                        kernel_config);
                    kernel_config_loaded = true;
                    if (!parse_ok) {
                        log_message(LogLevel::Warn,
                                    "Boot: KERNEL.CFG parse reported errors");
                    }
                    const char* init_spec = nullptr;
                    if (config::get(kernel_config, "KERNEL.INIT_TASK", init_spec)) {
                        if (build_mount_path(init_spec,
                                             root_ptr,
                                             init_task_path,
                                             sizeof(init_task_path))) {
                            init_task_path_valid = true;
                            log_message(LogLevel::Info,
                                        "Boot: init task set to %s",
                                        init_task_path);
                        } else {
                            log_message(LogLevel::Warn,
                                        "Boot: invalid KERNEL.INIT_TASK value '%s'",
                                        init_spec);
                        }
                    }
                } else {
                    log_message(LogLevel::Debug,
                                "VFS: %s not present or read failed", path);
                }
        }
    }
} else {
    log_message(LogLevel::Warn,
                "Boot: skipping KERNEL.CFG lookup (root not mounted)");
}

if (root_ptr != nullptr && root_ok && !kernel_config_loaded) {
    log_message(LogLevel::Warn,
                "Boot: KERNEL.CFG not found on '%s'",
                root_ptr);
}

process::init();
scheduler::init();

constexpr size_t kInitMaxSize = 64 * 1024;
alignas(16) static uint8_t init_buffer[kInitMaxSize];
size_t init_size = 0;

constexpr size_t kMaxInitCandidates = 4;
const char* init_candidates[kMaxInitCandidates];
size_t init_candidate_count = 0;

if (init_task_path_valid) {
    init_candidates[init_candidate_count++] = init_task_path;
}

char default_init_paths[2][64] = {{0}};
size_t default_paths_used = 0;

if (root_ptr != nullptr) {
    constexpr const char* kDefaultInitFiles[] = {"init.elf", "init.bin"};

    for (const char* fallback : kDefaultInitFiles) {
        if (default_paths_used >= 2) {
            break;
        }

        char* buffer = default_init_paths[default_paths_used];
        string_util::copy(buffer, sizeof(default_init_paths[default_paths_used]),
                          root_ptr);
        size_t len = string_util::length(buffer);
        if (len + 1 >= sizeof(default_init_paths[default_paths_used])) {
            log_message(LogLevel::Warn,
                        "Boot: init path truncated for root mount '%s'",
                        root_ptr);
            continue;
        }
        buffer[len++] = '/';

        size_t fallback_len = string_util::length(fallback);
        if (len + fallback_len >= sizeof(default_init_paths[default_paths_used])) {
            log_message(LogLevel::Warn,
                        "Boot: init filename '%s' truncated for root mount '%s'",
                        fallback,
                        root_ptr);
            continue;
        }
        for (size_t idx = 0; idx < fallback_len; ++idx) {
            buffer[len + idx] = fallback[idx];
        }
        buffer[len + fallback_len] = '\0';

        bool duplicate = init_task_path_valid &&
                         string_util::equals(init_task_path, buffer);
        if (!duplicate && init_candidate_count < kMaxInitCandidates) {
            init_candidates[init_candidate_count++] = buffer;
        }
        ++default_paths_used;
    }
}

bool init_loaded = false;
const char* init_path_used = nullptr;

for (size_t i = 0; i < init_candidate_count; ++i) {
    const char* candidate = init_candidates[i];
    if (candidate == nullptr) {
        continue;
    }
    if (vfs::read_file(candidate, init_buffer, sizeof(init_buffer),
                       init_size)) {
        init_loaded = true;
        init_path_used = candidate;
        break;
    }
    log_message(LogLevel::Warn,
                "Boot: init task not found at %s",
                candidate);
}

if (init_loaded) {
    loader::ProgramImage image{init_buffer, init_size, 0};
    process::Process* proc = process::allocate();
    bool init_started = false;
    if (proc != nullptr) {
        string_util::copy(proc->cwd, sizeof(proc->cwd), boot_cwd);
        if (loader::load_into_process(image, *proc)) {
            log_message(LogLevel::Info,
                        "Boot: launched init task from %s (%x bytes)",
                        init_path_used,
                        init_size);
            scheduler::enqueue(proc);
            init_started = true;
        } else {
            proc->state = process::State::Unused;
            proc->pid = 0;
        }
    }
    if (!init_started) {
        log_message(LogLevel::Error,
                    "Boot: failed to start init task (%s)",
                    init_path_used != nullptr ? init_path_used : "(unknown)");
    }
} else if (init_candidate_count > 0) {
    log_message(LogLevel::Error,
                "Boot: no init task could be loaded");
} else {
    log_message(LogLevel::Warn,
                "Boot: init task not attempted (no path configured)");
}

    scheduler::run();

    log_message(LogLevel::Error,
                "Scheduler failed to start or was exited, halting");
    hcf();
}
