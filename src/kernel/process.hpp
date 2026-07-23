#pragma once

#include <stddef.h>
#include <stdint.h>

#include "arch/x86_64/cpu_features.hpp"
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
    Allocating,
    Reclaiming,
    Waking,
};

struct FileHandle {
    bool in_use;
    bool can_write;
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
    alignas(cpu::kFpuStateAlign) uint8_t fpu_state[cpu::kFpuStateSize];
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
    bool reclaim_pending;
    uint32_t reclaim_cpu;
    void (*kernel_entry)(Process&);
    uint32_t preferred_cpu;  // UINT32_MAX means unassigned
    uint32_t vty_id;
    uint64_t sleep_until_tick;
    uint64_t user_ticks;
    uint64_t kernel_ticks;
    uint64_t wait_descriptors_user;
    uint32_t wait_descriptor_count;
    uint32_t wait_descriptor_reserved;
    int64_t wait_result;
    bool wait_result_pending;
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

inline State load_state(const Process& proc) {
    return __atomic_load_n(&proc.state, __ATOMIC_ACQUIRE);
}

inline void store_state(Process& proc, State state) {
    __atomic_store_n(&proc.state, state, __ATOMIC_RELEASE);
}

inline bool compare_exchange_state(Process& proc,
                                   State& expected,
                                   State desired) {
    return __atomic_compare_exchange_n(&proc.state,
                                       &expected,
                                       desired,
                                       false,
                                       __ATOMIC_ACQ_REL,
                                       __ATOMIC_ACQUIRE);
}

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
bool wake(Process& proc);
bool begin_wake(Process& proc);
void finish_wake(Process& proc);
void finish_wake_with_result(Process& proc, int64_t result);
bool wake_with_result(Process& proc, int64_t result);
void terminate(Process& proc, uint16_t exit_code);
bool consume_wait_result(Process& proc, int64_t& out_result);
void defer_reclaim(Process& proc);
void reap_deferred();
void reclaim(Process& proc);

}  // namespace process
