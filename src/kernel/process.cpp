#include "process.hpp"

#include "arch/x86_64/percpu.hpp"
#include "arch/x86_64/registers.hpp"
#include "arch/x86_64/memory/paging.hpp"
#include "capabilities.hpp"
#include "lib/mem.hpp"
#include "scheduler.hpp"
#include "string_util.hpp"

namespace {

process::Process g_process_table[process::kMaxProcesses];
alignas(16) uint8_t g_kernel_stacks[process::kMaxProcesses][process::kKernelStackSize];
uint32_t g_next_pid = 1;
bool g_init_pid_reserved = true;

constexpr uint32_t kInitPid = 1;

bool running_on_process_stack(const process::Process& proc) {
    uint64_t rsp = 0;
    asm volatile("mov %%rsp, %0" : "=r"(rsp));
    return rsp >= proc.kernel_stack_base && rsp < proc.kernel_stack_top;
}

void reset_process_resources(process::Process& proc) {
    proc.fs_base = 0;
    proc.user_ip = 0;
    proc.user_sp = 0;
    cpu::init_fpu_state(proc.fpu_state);
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
    proc.reclaim_pending = false;
    proc.reclaim_cpu = UINT32_MAX;
    proc.kernel_entry = nullptr;
    proc.preferred_cpu = UINT32_MAX;
    proc.vty_id = 0;
    proc.sleep_until_tick = 0;
    proc.user_ticks = 0;
    proc.kernel_ticks = 0;
    proc.wait_descriptors_user = 0;
    proc.wait_descriptor_count = 0;
    proc.wait_descriptor_reserved = 0;
    proc.wait_result = 0;
    proc.wait_result_pending = false;
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
        proc.file_handles[fh].can_write = false;
        proc.file_handles[fh].handle = {};
        proc.file_handles[fh].position = 0;
    }
    for (size_t dh = 0; dh < process::kMaxDirectoryHandles; ++dh) {
        proc.directory_handles[dh].in_use = false;
        proc.directory_handles[dh].handle = {};
        proc.directory_handles[dh].path[0] = '\0';
    }
}

bool pid_in_use(uint32_t pid) {
    if (pid == 0) {
        return true;
    }
    for (size_t i = 0; i < process::kMaxProcesses; ++i) {
        const process::Process& proc = g_process_table[i];
        process::State state = process::load_state(proc);
        if (state != process::State::Unused && proc.pid == pid) {
            return true;
        }
    }
    return false;
}

uint32_t allocate_pid() {
    for (uint32_t attempts = 0; attempts < UINT32_MAX; ++attempts) {
        uint32_t pid = __atomic_fetch_add(&g_next_pid,
                                          uint32_t{1},
                                          __ATOMIC_RELAXED);
        if (pid == 0 ||
            (__atomic_load_n(&g_init_pid_reserved, __ATOMIC_ACQUIRE) &&
             pid == kInitPid)) {
            continue;
        }
        if (!pid_in_use(pid)) {
            return pid;
        }
    }
    return 0;
}

process::Process* allocate_slot(uint32_t pid) {
    if (pid == 0 || pid_in_use(pid)) {
        return nullptr;
    }
    for (size_t i = 0; i < process::kMaxProcesses; ++i) {
        process::Process& proc = g_process_table[i];
        process::State expected = process::State::Unused;
        if (!process::compare_exchange_state(proc,
                                             expected,
                                             process::State::Allocating)) {
            continue;
        }
        memset(&proc.context, 0, sizeof(proc.context));
        reset_process_resources(proc);
        proc.pid = pid;
        return &proc;
    }
    return nullptr;
}

}  // namespace

namespace process {

void init() {
    memset(g_process_table, 0, sizeof(g_process_table));
    g_next_pid = 1;
    g_init_pid_reserved = true;
    for (size_t i = 0; i < kMaxProcesses; ++i) {
        store_state(g_process_table[i], State::Unused);
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
    Process* proc = allocate_slot(allocate_pid());
    if (proc == nullptr) {
        return nullptr;
    }
    uint64_t new_cr3 = paging_create_address_space();
    if (new_cr3 == 0) {
        proc->pid = 0;
        store_state(*proc, State::Unused);
        return nullptr;
    }
    proc->cr3 = new_cr3;
    store_state(*proc, State::Ready);
    return proc;
}

Process* allocate_init_task() {
    if (!__atomic_load_n(&g_init_pid_reserved, __ATOMIC_ACQUIRE) ||
        pid_in_use(kInitPid)) {
        return nullptr;
    }
    Process* proc = allocate_slot(kInitPid);
    if (proc == nullptr) {
        return nullptr;
    }
    uint64_t new_cr3 = paging_create_address_space();
    if (new_cr3 == 0) {
        proc->pid = 0;
        store_state(*proc, State::Unused);
        return nullptr;
    }
    proc->cr3 = new_cr3;
    store_state(*proc, State::Ready);
    __atomic_store_n(&g_init_pid_reserved, false, __ATOMIC_RELEASE);
    uint32_t next_pid = __atomic_load_n(&g_next_pid, __ATOMIC_RELAXED);
    while (next_pid <= kInitPid &&
           !__atomic_compare_exchange_n(&g_next_pid,
                                        &next_pid,
                                        kInitPid + 1,
                                        false,
                                        __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED)) {
    }
    return proc;
}

Process* current() {
    return percpu::get_current_process();
}

void set_current(Process* proc) {
    percpu::set_current_process(proc);
    if (proc != nullptr) {
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
        if (load_state(p) != State::Unused && p.pid == pid) {
            return &p;
        }
    }
    return nullptr;
}

void record_tick(bool user_mode) {
    Process* proc = current();
    if (proc == nullptr) {
        return;
    }
    if (user_mode) {
        ++proc->user_ticks;
    } else {
        ++proc->kernel_ticks;
    }
}

size_t usage_snapshot(descriptor_defs::TaskUsage* out, size_t max_entries) {
    if (out == nullptr || max_entries == 0) {
        return 0;
    }

    size_t written = 0;
    for (size_t i = 0; i < kMaxProcesses && written < max_entries; ++i) {
        const Process& proc = g_process_table[i];
        State state = load_state(proc);
        if (state == State::Unused || proc.pid == 0) {
            continue;
        }

        descriptor_defs::TaskUsage& snapshot = out[written++];
        snapshot.pid = proc.pid;
        snapshot.parent_pid = proc.parent ? proc.parent->pid : 0;
        snapshot.state = static_cast<uint32_t>(state);
        snapshot.flags = 0;
        if (proc.is_kernel_task) {
            snapshot.flags |= descriptor_defs::kTaskStatFlagKernel;
        }
        if (proc.has_exited) {
            snapshot.flags |= descriptor_defs::kTaskStatFlagExited;
        }
        snapshot.preferred_cpu = proc.preferred_cpu;
        snapshot.reserved0 = 0;
        snapshot.user_ticks = proc.user_ticks;
        snapshot.kernel_ticks = proc.kernel_ticks;
        string_util::copy(snapshot.image_path,
                          sizeof(snapshot.image_path),
                          proc.image_path[0] != '\0' ? proc.image_path : "(kernel)");
    }
    return written;
}

void wake_ready_sleepers(uint64_t current_tick) {
    for (size_t i = 0; i < kMaxProcesses; ++i) {
        Process& proc = g_process_table[i];
        if (load_state(proc) != State::Blocked ||
            proc.sleep_until_tick == 0) {
            continue;
        }
        if (current_tick < proc.sleep_until_tick) {
            continue;
        }
        proc.sleep_until_tick = 0;
        (void)wake(proc);
    }
}

bool wake(Process& proc) {
    if (!begin_wake(proc)) {
        return false;
    }
    finish_wake(proc);
    return true;
}

bool begin_wake(Process& proc) {
    State expected = State::Blocked;
    if (!compare_exchange_state(proc, expected, State::Waking)) {
        return false;
    }
    return true;
}

void finish_wake(Process& proc) {
    proc.waiting_on = nullptr;
    store_state(proc, State::Ready);
    scheduler::enqueue(&proc);
}

void finish_wake_with_result(Process& proc, int64_t result) {
    proc.wait_result = result;
    __atomic_store_n(&proc.wait_result_pending, true, __ATOMIC_RELEASE);
    finish_wake(proc);
}

bool wake_with_result(Process& proc, int64_t result) {
    if (!begin_wake(proc)) {
        return false;
    }
    finish_wake_with_result(proc, result);
    return true;
}

void terminate(Process& proc, uint16_t exit_code) {
    State state = load_state(proc);
    for (;;) {
        if (state == State::Waking) {
            asm volatile("pause");
            state = load_state(proc);
            continue;
        }
        if (state == State::Unused || state == State::Allocating ||
            state == State::Terminated || state == State::Reclaiming) {
            return;
        }
        if (compare_exchange_state(proc, state, State::Terminated)) {
            break;
        }
    }
    proc.has_exited = true;
    proc.exit_code = exit_code;

    Process* parent = proc.parent;
    if (parent != nullptr && parent->waiting_on == &proc) {
        if (begin_wake(*parent)) {
            if (parent->console_transferred) {
                descriptor::restore_console_owner(*parent);
                parent->console_transferred = false;
            }
            finish_wake_with_result(*parent, proc.exit_code);
        }
    }
    proc.parent = nullptr;
}

bool consume_wait_result(Process& proc, int64_t& out_result) {
    if (!__atomic_exchange_n(&proc.wait_result_pending,
                             false,
                             __ATOMIC_ACQ_REL)) {
        return false;
    }
    out_result = proc.wait_result;
    return true;
}

void defer_reclaim(Process& proc) {
    State state = load_state(proc);
    if (state != State::Terminated) {
        return;
    }
    const percpu::Cpu* cpu = percpu::current_cpu();
    if (cpu == nullptr || cpu->index >= percpu::kMaxCpus) {
        return;
    }
    __atomic_store_n(&proc.reclaim_cpu, cpu->index, __ATOMIC_RELAXED);
    __atomic_store_n(&proc.reclaim_pending, true, __ATOMIC_RELEASE);
}

void reap_deferred() {
    const percpu::Cpu* cpu = percpu::current_cpu();
    if (cpu == nullptr || cpu->index >= percpu::kMaxCpus) {
        return;
    }
    for (size_t i = 0; i < kMaxProcesses; ++i) {
        Process& proc = g_process_table[i];
        if (!__atomic_load_n(&proc.reclaim_pending, __ATOMIC_ACQUIRE) ||
            __atomic_load_n(&proc.reclaim_cpu, __ATOMIC_RELAXED) != cpu->index ||
            running_on_process_stack(proc)) {
            continue;
        }
        bool expected = true;
        if (!__atomic_compare_exchange_n(&proc.reclaim_pending,
                                         &expected,
                                         false,
                                         false,
                                         __ATOMIC_ACQ_REL,
                                         __ATOMIC_ACQUIRE)) {
            continue;
        }
        reclaim(proc);
    }
}

Process* allocate_kernel_task(void (*entry)(Process&)) {
    if (entry == nullptr) {
        return nullptr;
    }
    Process* proc = allocate_slot(allocate_pid());
    if (proc == nullptr) {
        return nullptr;
    }
    proc->cr3 = paging_kernel_cr3();
    proc->is_kernel_task = true;
    proc->kernel_entry = entry;
    store_state(*proc, State::Ready);
    return proc;
}

void reclaim(Process& proc) {
    if (running_on_process_stack(proc)) {
        defer_reclaim(proc);
        return;
    }

    State state = load_state(proc);
    for (;;) {
        if (state != State::Ready && state != State::Terminated) {
            return;
        }
        if (compare_exchange_state(proc, state, State::Reclaiming)) {
            break;
        }
    }
    __atomic_store_n(&proc.reclaim_pending, false, __ATOMIC_RELEASE);
    __atomic_store_n(&proc.reclaim_cpu, UINT32_MAX, __ATOMIC_RELAXED);
    scheduler::remove(&proc);

    for (size_t i = 0; i < kMaxFileHandles; ++i) {
        if (proc.file_handles[i].in_use) {
            vfs::close_file(proc.file_handles[i].handle);
            proc.file_handles[i].in_use = false;
            proc.file_handles[i].can_write = false;
        }
    }
    for (size_t i = 0; i < kMaxDirectoryHandles; ++i) {
        if (proc.directory_handles[i].in_use) {
            vfs::close_directory(proc.directory_handles[i].handle);
            proc.directory_handles[i].in_use = false;
        }
    }
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

    // Do not leave children pointing at a slot that can be reused for an
    // unrelated process.
    for (size_t i = 0; i < kMaxProcesses; ++i) {
        if (g_process_table[i].parent == &proc) {
            g_process_table[i].parent = nullptr;
        }
    }

    proc.pid = 0;
    proc.cr3 = paging_kernel_cr3();
    reset_process_resources(proc);
    store_state(proc, State::Unused);
}

}  // namespace process
