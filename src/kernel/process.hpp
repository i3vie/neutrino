#pragma once

#include <stddef.h>
#include <stdint.h>

#include "arch/x86_64/syscall.hpp"
#include "descriptor.hpp"
#include "fs/vfs.hpp"
#include "path_util.hpp"
#include "capabilities.hpp"
#include "vm.hpp"

namespace process {

constexpr size_t kMaxProcesses = 256;
constexpr size_t kKernelStackSize = 0x4000;
constexpr size_t kMaxFileHandles = 16;
constexpr size_t kMaxDirectoryHandles = 8;

enum class State {
    Unused = 0,
    Ready,
    Running,
    Blocked,
    Terminated,
};

struct FileHandle {
    bool in_use;
    vfs::FileHandle handle;
    uint64_t position;
};

struct DirectoryHandle {
    bool in_use;
    vfs::DirectoryHandle handle;
    char path[path_util::kMaxPathLength];
};

struct Process {
    uint32_t pid;
    State state;
    uint64_t cr3;
    uint64_t fs_base;
    uint64_t user_ip;
    uint64_t user_sp;
    uint64_t kernel_stack_base;
    uint64_t kernel_stack_top;
    vm::Region code_region;
    vm::Stack stack_region;
    syscall::SyscallFrame context;
    Process* parent;
    void* waiting_on;
    uint16_t exit_code;
    bool has_exited;
    bool console_transferred;
    bool has_context;
    bool is_kernel_task;
    void (*kernel_entry)(Process&);
    uint32_t preferred_cpu;  // UINT32_MAX means unassigned
    uint32_t vty_id;
    uint64_t sleep_until_tick;
    uint64_t user_ticks;
    uint64_t kernel_ticks;
    uint64_t wait_descriptors_user;
    uint32_t wait_descriptor_count;
    uint32_t wait_descriptor_reserved;
    char cwd[128];
    char image_path[path_util::kMaxPathLength];
    uint32_t standard_descriptors[3];
    descriptor::Table descriptors;
    descriptor_defs::DescriptorWait
        wait_descriptors[descriptor::kMaxWaitDescriptors];
    capabilities::Principal* principal;
    capabilities::CapHandleEntry cap_handles[capabilities::kMaxProcessCapabilities];
    FileHandle file_handles[kMaxFileHandles];
    DirectoryHandle directory_handles[kMaxDirectoryHandles];
};

void init();
Process* allocate();
Process* allocate_init_task();
Process* allocate_kernel_task(void (*entry)(Process&));
Process* current();
void set_current(Process* proc);
Process* table_entry(size_t index);
Process* find_by_pid(uint32_t pid);
void record_tick(bool user_mode);
size_t usage_snapshot(descriptor_defs::TaskUsage* out, size_t max_entries);
void wake_ready_sleepers(uint64_t current_tick);
void reclaim(Process& proc);

}  // namespace process
