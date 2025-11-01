#include "process.hpp"

#include "lib/mem.hpp"

namespace {

process::Process g_process_table[process::kMaxProcesses];
alignas(16) uint8_t g_kernel_stacks[process::kMaxProcesses][process::kKernelStackSize];
uint32_t g_next_pid = 1;
process::Process* g_current_process = nullptr;

}  // namespace

namespace process {

void init() {
    memset(g_process_table, 0, sizeof(g_process_table));
    g_next_pid = 1;
    g_current_process = nullptr;
    for (size_t i = 0; i < kMaxProcesses; ++i) {
        g_process_table[i].state = State::Unused;
        g_process_table[i].has_context = false;
        g_process_table[i].pid = 0;
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
        memset(&proc.context, 0, sizeof(proc.context));
        proc.state = State::Ready;
        proc.pid = g_next_pid++;
        proc.cr3 = 0;
        proc.has_context = false;
        proc.parent = nullptr;
        proc.waiting_on = nullptr;
        proc.exit_code = 0;
        proc.has_exited = false;
        proc.console_transferred = false;
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
    return g_current_process;
}

void set_current(Process* proc) {
    g_current_process = proc;
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

}  // namespace process
