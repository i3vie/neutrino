#pragma once

#include <stddef.h>
#include <stdint.h>

#include "arch/x86_64/syscall.hpp"
#include "vm.hpp"

namespace process {

constexpr size_t kMaxProcesses = 16;
constexpr size_t kKernelStackSize = 0x4000;

enum class State {
    Unused = 0,
    Ready,
    Running,
    Blocked,
    Terminated,
};

struct Process {
    uint32_t pid;
    State state;
    uint64_t cr3;
    uint64_t user_ip;
    uint64_t user_sp;
    uint64_t kernel_stack_base;
    uint64_t kernel_stack_top;
    vm::Region code_region;
    vm::Stack stack_region;
    syscall::SyscallFrame context;
    Process* parent;
    bool has_context;
};

void init();
Process* allocate();
Process* current();
void set_current(Process* proc);
Process* table_entry(size_t index);

}  // namespace process

