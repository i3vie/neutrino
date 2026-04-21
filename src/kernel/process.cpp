#include "process.hpp"

#include "arch/x86_64/percpu.hpp"
#include "arch/x86_64/registers.hpp"
#include "arch/x86_64/memory/paging.hpp"
#include "capabilities.hpp"
#include "lib/mem.hpp"

namespace {

process::Process g_process_table[process::kMaxProcesses];
alignas(16) uint8_t g_kernel_stacks[process::kMaxProcesses][process::kKernelStackSize];
uint32_t g_next_pid = 1;
uint32_t g_next_kernel_pid = 0x80000000u;

void reset_process_resources(process::Process& proc) {
    proc.fs_base = 0;
    proc.user_ip = 0;
    proc.user_sp = 0;
    proc.code_region = vm::Region{0, 0};
    proc.stack_region = vm::Stack{0, 0, 0};
    memset(&proc.context, 0, sizeof(proc.context));
    proc.parent = nullptr;
    proc.waiting_on = nullptr;
    proc.exit_code = 0;
    proc.has_exited = false;
    proc.console_transferred = false;
    proc.has_context = false;
    proc.is_kernel_task = false;
    proc.kernel_entry = nullptr;
    proc.preferred_cpu = UINT32_MAX;
    proc.vty_id = 0;
    proc.cwd[0] = '/';
    proc.cwd[1] = '\0';
    proc.image_path[0] = '\0';
    for (size_t i = 0; i < 3; ++i) {
        proc.standard_descriptors[i] = descriptor::kInvalidHandle;
    }
    proc.principal = nullptr;
    capabilities::cap_table_clear(proc.cap_handles,
                                  capabilities::kMaxProcessCapabilities);
    descriptor::init_table(proc.descriptors);
    for (size_t fh = 0; fh < process::kMaxFileHandles; ++fh) {
        proc.file_handles[fh].in_use = false;
        proc.file_handles[fh].handle = {};
        proc.file_handles[fh].position = 0;
    }
    for (size_t dh = 0; dh < process::kMaxDirectoryHandles; ++dh) {
        proc.directory_handles[dh].in_use = false;
        proc.directory_handles[dh].handle = {};
        proc.directory_handles[dh].path[0] = '\0';
    }
}

}  // namespace

namespace process {

void init() {
    memset(g_process_table, 0, sizeof(g_process_table));
    g_next_pid = 1;
    for (size_t i = 0; i < kMaxProcesses; ++i) {
        g_process_table[i].state = State::Unused;
        g_process_table[i].has_context = false;
        g_process_table[i].is_kernel_task = false;
        g_process_table[i].kernel_entry = nullptr;
        g_process_table[i].pid = 0;
        g_process_table[i].cr3 = paging_kernel_cr3();
        g_process_table[i].kernel_stack_base =
            reinterpret_cast<uint64_t>(&g_kernel_stacks[i][0]);
        g_process_table[i].kernel_stack_top =
            g_process_table[i].kernel_stack_base + kKernelStackSize;
        g_process_table[i].kernel_stack_top &= ~0xFULL;
        reset_process_resources(g_process_table[i]);
    }
}

Process* allocate() {
    for (size_t i = 0; i < kMaxProcesses; ++i) {
        Process& proc = g_process_table[i];
        if (proc.state != State::Unused) {
            continue;
        }
        memset(&proc.context, 0, sizeof(proc.context));
        proc.state = State::Ready;
        proc.pid = g_next_kernel_pid++;
        uint64_t new_cr3 = paging_create_address_space();
        if (new_cr3 == 0) {
            proc.state = State::Unused;
            proc.pid = 0;
            return nullptr;
        }
        proc.cr3 = new_cr3;
        reset_process_resources(proc);
        return &proc;
    }
    return nullptr;
}

Process* current() {
    return percpu::get_current_process();
}

void set_current(Process* proc) {
    percpu::set_current_process(proc);
    if (proc != nullptr) {
        proc->state = State::Running;
        uint64_t target_cr3 =
            (proc->cr3 != 0) ? proc->cr3 : paging_kernel_cr3();
        if (target_cr3 != 0) {
            paging_switch_cr3(target_cr3);
        }
        cpu::write_fs_base(proc->fs_base);
    }
}

Process* table_entry(size_t index) {
    if (index >= kMaxProcesses) {
        return nullptr;
    }
    return &g_process_table[index];
}

Process* find_by_pid(uint32_t pid) {
    if (pid == 0) {
        return nullptr;
    }
    for (size_t i = 0; i < kMaxProcesses; ++i) {
        Process& p = g_process_table[i];
        if (p.state != State::Unused && p.pid == pid) {
            return &p;
        }
    }
    return nullptr;
}

Process* allocate_kernel_task(void (*entry)(Process&)) {
    if (entry == nullptr) {
        return nullptr;
    }
    for (size_t i = 0; i < kMaxProcesses; ++i) {
        Process& proc = g_process_table[i];
        if (proc.state != State::Unused) {
            continue;
        }
        memset(&proc.context, 0, sizeof(proc.context));
        proc.state = State::Ready;
        proc.pid = g_next_pid++;
        proc.cr3 = paging_kernel_cr3();
        reset_process_resources(proc);
        proc.is_kernel_task = true;
        proc.kernel_entry = entry;
        return &proc;
    }
    return nullptr;
}

void reclaim(Process& proc) {
    descriptor::destroy_table(proc, proc.descriptors);
    vm::release_user_region(proc.cr3, proc.code_region);
    vm::release_user_region(proc.cr3,
                            vm::Region{proc.stack_region.base,
                                       proc.stack_region.length});
    if (proc.principal != nullptr) {
        capabilities::principal_release(proc.principal);
    }
    if (!proc.is_kernel_task && proc.cr3 != 0) {
        vm::release_address_space(proc.cr3);
        paging_destroy_address_space(proc.cr3);
    }

    proc.state = State::Unused;
    proc.pid = 0;
    proc.cr3 = paging_kernel_cr3();
    reset_process_resources(proc);
}

}  // namespace process
