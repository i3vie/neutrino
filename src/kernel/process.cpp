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
