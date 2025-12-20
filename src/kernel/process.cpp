#include "process.hpp"

#include "arch/x86_64/memory/paging.hpp"
#include "arch/x86_64/percpu.hpp"
#include "vm.hpp"
#include "lib/mem.hpp"

namespace {

process::Process g_process_table[process::kMaxProcesses];
alignas(16) uint8_t g_kernel_stacks[process::kMaxProcesses][process::kKernelStackSize];
uint32_t g_next_pid = 1;

}  // namespace

namespace process {

void init() {
    memset(g_process_table, 0, sizeof(g_process_table));
    g_next_pid = 1;
    for (size_t i = 0; i < kMaxProcesses; ++i) {
        g_process_table[i].state = State::Unused;
        g_process_table[i].has_context = false;
        g_process_table[i].pid = 0;
        g_process_table[i].cr3 = 0;
        g_process_table[i].kernel_stack_base =
            reinterpret_cast<uint64_t>(&g_kernel_stacks[i][0]);
        g_process_table[i].kernel_stack_top =
            g_process_table[i].kernel_stack_base + kKernelStackSize;
        g_process_table[i].kernel_stack_top &= ~0xFULL;
        g_process_table[i].parent = nullptr;
        g_process_table[i].waiting_on = nullptr;
        g_process_table[i].exit_code = 0;
        g_process_table[i].has_exited = false;
        g_process_table[i].console_transferred = false;
        g_process_table[i].preferred_cpu = UINT32_MAX;
        g_process_table[i].next_code_cursor = vm::kUserAddressSpaceBase;
        g_process_table[i].next_stack_cursor = vm::kUserAddressSpaceTop;
        g_process_table[i].next_shared_cursor = vm::kUserAddressSpaceTop;
        g_process_table[i].cwd[0] = '/';
        g_process_table[i].cwd[1] = '\0';
        descriptor::init_table(g_process_table[i].descriptors);
        for (size_t fh = 0; fh < kMaxFileHandles; ++fh) {
            g_process_table[i].file_handles[fh].in_use = false;
            g_process_table[i].file_handles[fh].handle = {};
            g_process_table[i].file_handles[fh].position = 0;
        }
        for (size_t dh = 0; dh < kMaxDirectoryHandles; ++dh) {
            g_process_table[i].directory_handles[dh].in_use = false;
            g_process_table[i].directory_handles[dh].handle = {};
        }
    }
}

Process* allocate() {
    for (size_t i = 0; i < kMaxProcesses; ++i) {
        Process& proc = g_process_table[i];
        if (proc.state != State::Unused) {
            continue;
        }
        if (proc.cr3 == 0) {
            uint64_t cr3_value = paging_create_address_space();
            if (cr3_value == 0) {
                return nullptr;
            }
            proc.cr3 = cr3_value;
        } else {
            paging_reset_address_space(proc.cr3);
        }
        vm::reset_address_space_cursors(proc.cr3);
        memset(&proc.context, 0, sizeof(proc.context));
        proc.state = State::Ready;
        proc.pid = g_next_pid++;
        proc.has_context = false;
        proc.preferred_cpu = UINT32_MAX;
        proc.parent = nullptr;
        proc.waiting_on = nullptr;
        proc.exit_code = 0;
        proc.has_exited = false;
        proc.console_transferred = false;
        proc.next_code_cursor = vm::kUserAddressSpaceBase;
        proc.next_stack_cursor = vm::kUserAddressSpaceTop;
        proc.next_shared_cursor = vm::kUserAddressSpaceTop;
        proc.cwd[0] = '/';
        proc.cwd[1] = '\0';
        descriptor::init_table(proc.descriptors);
        for (size_t fh = 0; fh < kMaxFileHandles; ++fh) {
            proc.file_handles[fh].in_use = false;
            proc.file_handles[fh].handle = {};
            proc.file_handles[fh].position = 0;
        }
        for (size_t dh = 0; dh < kMaxDirectoryHandles; ++dh) {
            proc.directory_handles[dh].in_use = false;
            proc.directory_handles[dh].handle = {};
        }
        return &proc;
    }
    return nullptr;
}

Process* current() {
    return percpu::get_current_process();
}

void set_current(Process* proc) {
    percpu::set_current_process(proc);
    uint64_t target_cr3 = paging_cr3();
    if (proc != nullptr && proc->cr3 != 0) {
        target_cr3 = proc->cr3;
    }
    asm volatile("mov %0, %%cr3" : : "r"(target_cr3) : "memory");
    if (proc != nullptr) {
        proc->state = State::Running;
    }
}

Process* table_entry(size_t index) {
    if (index >= kMaxProcesses) {
        return nullptr;
    }
    return &g_process_table[index];
}

Process* find_by_cr3(uint64_t cr3) {
    if (cr3 == 0) {
        return nullptr;
    }
    for (size_t i = 0; i < kMaxProcesses; ++i) {
        Process* proc = &g_process_table[i];
        if (proc->cr3 == cr3 && proc->state != State::Unused) {
            return proc;
        }
    }
    return nullptr;
}

}  // namespace process

extern "C" uint64_t syscall_kernel_stack() {
    process::Process* proc = process::current();
    if (proc == nullptr) {
        return 0;
    }
    return proc->kernel_stack_top;
}
