#include "scheduler.hpp"

#include <stdint.h>

#include "drivers/log/logging.hpp"
#include "lib/mem.hpp"
#include "arch/x86_64/gdt.hpp"
#include "arch/x86_64/tss.hpp"
#include "arch/x86_64/registers.hpp"
#include "arch/x86_64/memory/paging.hpp"
#include "userspace.hpp"
#include "arch/x86_64/percpu.hpp"
#include "arch/x86_64/smp.hpp"
#include "descriptor.hpp"
#include "time.hpp"

namespace {

struct RunQueue {
    process::Process* items[process::kMaxProcesses];
    size_t head = 0;
    size_t count = 0;
};

RunQueue g_run_queues[percpu::kMaxCpus];
size_t g_cpu_total = 0;
volatile int g_queue_lock = 0;
uint32_t g_rr_assign = 0;
constexpr size_t kMaxPollFns = 16;
scheduler::PollFn g_poll_fns[kMaxPollFns]{};
volatile int g_poll_lock = 0;
process::Process* g_poll_worker = nullptr;

[[maybe_unused]] void halt_system() {
    for (;;) {
        asm volatile("hlt");
    }
}

RunQueue& queue_for_cpu(size_t idx) {
    if (idx >= percpu::kMaxCpus) {
        return g_run_queues[0];
    }
    return g_run_queues[idx];
}

bool queue_contains(process::Process* proc) {
    for (size_t q = 0; q < percpu::kMaxCpus; ++q) {
        RunQueue& rq = g_run_queues[q];
        size_t idx = rq.head;
        for (size_t i = 0; i < rq.count; ++i) {
            if (rq.items[idx] == proc) {
                return true;
            }
            idx = (idx + 1) % process::kMaxProcesses;
        }
    }
    return false;
}

void queue_push(RunQueue& rq, process::Process* proc) {
    if (rq.count >= process::kMaxProcesses) {
        return;
    }
    size_t tail = (rq.head + rq.count) % process::kMaxProcesses;
    rq.items[tail] = proc;
    ++rq.count;
}

process::Process* queue_pop(RunQueue& rq) {
    if (rq.count == 0) {
        return nullptr;
    }
    process::Process* proc = rq.items[rq.head];
    rq.head = (rq.head + 1) % process::kMaxProcesses;
    --rq.count;
    return proc;
}

void lock_queue() {
    while (__atomic_test_and_set(&g_queue_lock, __ATOMIC_ACQUIRE)) {
        asm volatile("pause");
    }
}

void unlock_queue() {
    __atomic_clear(&g_queue_lock, __ATOMIC_RELEASE);
}

void lock_pollers() {
    while (__atomic_test_and_set(&g_poll_lock, __ATOMIC_ACQUIRE)) {
        asm volatile("pause");
    }
}

void unlock_pollers() {
    __atomic_clear(&g_poll_lock, __ATOMIC_RELEASE);
}

size_t poll_count_locked() {
    size_t count = 0;
    for (size_t i = 0; i < kMaxPollFns; ++i) {
        if (g_poll_fns[i] != nullptr) {
            ++count;
        }
    }
    return count;
}

class QueueGuard {
public:
    QueueGuard() { lock_queue(); }
    ~QueueGuard() { unlock_queue(); }
};

void poll_worker(process::Process& proc) {
    scheduler::service_polls();
    lock_pollers();
    bool has_pollers = poll_count_locked() != 0;
    unlock_pollers();
    proc.state = has_pollers ? process::State::Ready : process::State::Blocked;
}

void enqueue_locked(process::Process* proc) {
    size_t total = g_cpu_total;
    size_t online = smp::online_cpus();
    if (online != 0 && online < total) {
        total = online;
    }
    if (total == 0) {
        total = 1;
    }
    if (proc->preferred_cpu == UINT32_MAX || proc->preferred_cpu >= total) {
        uint32_t choice = __atomic_fetch_add(&g_rr_assign, 1, __ATOMIC_RELAXED);
        proc->preferred_cpu = static_cast<uint32_t>(choice % total);
    }
    size_t target = static_cast<size_t>(proc->preferred_cpu % total);
    queue_push(queue_for_cpu(target), proc);
}

process::Process* pop_locked() {
    percpu::Cpu* cpu = percpu::current_cpu();
    size_t idx = cpu ? cpu->index : 0;

    process::Process* proc = queue_pop(queue_for_cpu(idx));
    if (proc != nullptr) {
        return proc;
    }
    size_t total = g_cpu_total;
    size_t online = smp::online_cpus();
    if (online != 0 && online < total) {
        total = online;
    }
    for (size_t q = 0; q < total; ++q) {
        if (q == idx) continue;
        proc = queue_pop(queue_for_cpu(q));
        if (proc != nullptr) {
            return proc;
        }
    }
    return nullptr;
}

void prepare_frame_for_process(process::Process& proc,
                               syscall::SyscallFrame& frame) {
    if (proc.has_context) {
        frame = proc.context;
    } else {
        memset(&frame, 0, sizeof(frame));
        frame.user_rip = proc.user_ip;
        frame.user_rsp = proc.user_sp;
        frame.user_rflags = 0x202;
        frame.r11 = 0x202;
    }
    if (!proc.has_context) {
        log_message(LogLevel::Debug,
                    "Scheduler: starting pid=%u image=%s code=%016llx+%zu rip=%016llx stack=%016llx..%016llx rsp=%016llx",
                    static_cast<unsigned int>(proc.pid),
                    proc.image_path[0] != '\0' ? proc.image_path : "(unknown)",
                    static_cast<unsigned long long>(proc.code_region.base),
                    static_cast<size_t>(proc.code_region.length),
                    static_cast<unsigned long long>(proc.user_ip),
                    static_cast<unsigned long long>(proc.stack_region.base),
                    static_cast<unsigned long long>(proc.stack_region.top),
                    static_cast<unsigned long long>(proc.user_sp));
    }

    set_rsp0(proc.kernel_stack_top);
}

void capture_from_interrupt(const InterruptFrame& in,
                            syscall::SyscallFrame& out) {
    out.rax = in.rax;
    out.rbx = in.rbx;
    out.rcx = in.rcx;
    out.rdx = in.rdx;
    out.rsi = in.rsi;
    out.rdi = in.rdi;
    out.rbp = in.rbp;
    out.r8  = in.r8;
    out.r9  = in.r9;
    out.r10 = in.r10;
    out.r11 = in.r11;
    out.r12 = in.r12;
    out.r13 = in.r13;
    out.r14 = in.r14;
    out.r15 = in.r15;
    out.user_rip = in.rip;
    out.user_rsp = in.rsp;
    out.user_rflags = in.rflags;
}

void apply_to_interrupt(const syscall::SyscallFrame& in,
                        InterruptFrame& out) {
    out.rax = in.rax;
    out.rbx = in.rbx;
    out.rcx = in.rcx;
    out.rdx = in.rdx;
    out.rsi = in.rsi;
    out.rdi = in.rdi;
    out.rbp = in.rbp;
    out.r8  = in.r8;
    out.r9  = in.r9;
    out.r10 = in.r10;
    out.r11 = in.r11;
    out.r12 = in.r12;
    out.r13 = in.r13;
    out.r14 = in.r14;
    out.r15 = in.r15;
    out.rip = in.user_rip;
    out.rsp = in.user_rsp;
    out.rflags = in.user_rflags | 0x202;
    out.cs = USER_CS;
    out.ss = USER_DS;
}

}  // namespace

namespace scheduler {

void init() {
    for (size_t i = 0; i < percpu::kMaxCpus; ++i) {
        g_run_queues[i].head = 0;
        g_run_queues[i].count = 0;
    }
}

process::Process* current() {
    return process::current();
}

void register_cpu(percpu::Cpu* cpu) {
    if (cpu == nullptr) {
        return;
    }
    if (cpu->registered) {
        return;
    }
    size_t idx = __atomic_fetch_add(&g_cpu_total, 1, __ATOMIC_SEQ_CST);
    if (idx >= percpu::kMaxCpus) {
        __atomic_fetch_sub(&g_cpu_total, 1, __ATOMIC_SEQ_CST);
        return;
    }
    cpu->index = static_cast<uint32_t>(idx);
    g_run_queues[idx].head = 0;
    g_run_queues[idx].count = 0;
    cpu->registered = true;
    log_message(LogLevel::Info,
                "Scheduler: registered CPU (LAPIC=%u total=%u)",
                cpu->lapic_id,
                static_cast<unsigned int>(__atomic_load_n(&g_cpu_total, __ATOMIC_SEQ_CST)));
}

size_t cpu_total() {
    return __atomic_load_n(&g_cpu_total, __ATOMIC_SEQ_CST);
}

bool register_poll(PollFn fn) {
    if (fn == nullptr) {
        return false;
    }

    lock_pollers();
    for (size_t i = 0; i < kMaxPollFns; ++i) {
        if (g_poll_fns[i] == fn) {
            unlock_pollers();
            return true;
        }
    }
    for (size_t i = 0; i < kMaxPollFns; ++i) {
        if (g_poll_fns[i] == nullptr) {
            g_poll_fns[i] = fn;
            unlock_pollers();
            if (g_poll_worker == nullptr) {
                process::Process* worker = process::allocate_kernel_task(poll_worker);
                if (worker != nullptr) {
                    worker->preferred_cpu = 0;
                    g_poll_worker = worker;
                    enqueue(worker);
                }
            } else if (g_poll_worker->state == process::State::Blocked) {
                g_poll_worker->state = process::State::Ready;
                g_poll_worker->waiting_on = nullptr;
                enqueue(g_poll_worker);
            }
            return true;
        }
    }
    unlock_pollers();
    return false;
}

void service_polls() {
    percpu::Cpu* cpu = percpu::current_cpu();
    if (cpu != nullptr && cpu->index != 0) {
        return;
    }

    PollFn pollers[kMaxPollFns]{};
    size_t count = 0;

    lock_pollers();
    for (size_t i = 0; i < kMaxPollFns; ++i) {
        if (g_poll_fns[i] != nullptr) {
            pollers[count++] = g_poll_fns[i];
        }
    }
    unlock_pollers();

    for (size_t i = 0; i < count; ++i) {
        pollers[i]();
    }
}

void enqueue(process::Process* proc) {
    if (proc == nullptr) {
        return;
    }
    if (proc->state == process::State::Terminated) {
        return;
    }
    QueueGuard guard;
    if (!queue_contains(proc)) {
        enqueue_locked(proc);
    }
    proc->state = process::State::Ready;
}

[[noreturn]] static void run_loop() {
    for (;;) {
        process::Process* next = nullptr;
        {
            QueueGuard guard;
            next = pop_locked();
        }
        while (next != nullptr && next->state == process::State::Terminated) {
            process::reclaim(*next);
            QueueGuard guard;
            next = pop_locked();
        }
        if (next == nullptr) {
            process::set_current(nullptr);
            asm volatile("pause");
            asm volatile("sti; hlt");
            continue;
        }
        if (next->is_kernel_task) {
            process::set_current(next);
            set_rsp0(next->kernel_stack_top);
            asm volatile("sti");
            if (next->kernel_entry != nullptr) {
                next->kernel_entry(*next);
            }
            if (next->state == process::State::Ready) {
                QueueGuard guard;
                enqueue_locked(next);
            }
            continue;
        }
        if (next->state == process::State::Terminated) {
            continue;
        }
        process::set_current(next);
        set_rsp0(next->kernel_stack_top);
        userspace::enter_process(*next);
        log_message(LogLevel::Error,
                    "Scheduler: process %u returned unexpectedly",
                    static_cast<unsigned int>(next->pid));
    }
}

void reschedule(syscall::SyscallFrame& frame) {
    process::Process* current_proc = process::current();
    if (current_proc == nullptr) {
        return;
    }

    bool terminated = current_proc->state == process::State::Terminated;

    if (!terminated) {
        current_proc->context = frame;
        current_proc->has_context = true;
        current_proc->user_ip = frame.user_rip;
        current_proc->user_sp = frame.user_rsp;
        current_proc->fs_base = cpu::read_fs_base();
    } else {
        current_proc->has_context = false;
    }

    process::Process* next = nullptr;
    {
        QueueGuard guard;
        if (!terminated) {
            if (current_proc->state == process::State::Running) {
                current_proc->state = process::State::Ready;
            }
            if (current_proc->state == process::State::Ready &&
                !queue_contains(current_proc)) {
                enqueue_locked(current_proc);
            }
        }

        next = pop_locked();
        while (next != nullptr &&
               (next->state == process::State::Terminated)) {
            process::reclaim(*next);
            next = pop_locked();
        }
    }

    for (;;) {
        // Run any ready kernel tasks immediately (they have no userspace frame)
        while (next != nullptr && next->is_kernel_task) {
            process::set_current(next);
            set_rsp0(next->kernel_stack_top);
            if (next->kernel_entry != nullptr) {
                next->kernel_entry(*next);
            }
            if (next->state == process::State::Ready) {
                QueueGuard guard;
                enqueue_locked(next);
            }
            // fetch another runnable task (prefer userspace)
            {
                QueueGuard guard;
                next = pop_locked();
                while (next != nullptr &&
                       (next->state == process::State::Terminated)) {
                    process::reclaim(*next);
                    next = pop_locked();
                }
            }
        }

        if (next != nullptr) {
            break;
        }

        if (terminated) {
            process::set_current(nullptr);
            paging_switch_cr3(paging_kernel_cr3());
            process::reclaim(*current_proc);
            for (;;) {
                asm volatile("sti; hlt");
            }
        }

        if (current_proc->state == process::State::Ready ||
            current_proc->state == process::State::Running) {
            process::set_current(current_proc);
            prepare_frame_for_process(*current_proc, frame);
            asm volatile("pause");
            return;
        }

        process::set_current(nullptr);
        paging_switch_cr3(paging_kernel_cr3());
        cpu::write_fs_base(0);

        do {
            asm volatile("sti; hlt");
            QueueGuard guard;
            next = pop_locked();
            while (next != nullptr &&
                   next->state == process::State::Terminated) {
                process::reclaim(*next);
                next = pop_locked();
            }
        } while (next == nullptr);
    }

    process::set_current(next);
    if (terminated) {
        process::reclaim(*current_proc);
    }
    prepare_frame_for_process(*next, frame);
}

void reschedule_from_interrupt(InterruptFrame& frame) {
    if ((frame.cs & 0x3) == 0) {
        return;
    }

    syscall::SyscallFrame state{};
    capture_from_interrupt(frame, state);
    reschedule(state);
    apply_to_interrupt(state, frame);
}

void tick(InterruptFrame& frame) {
    descriptor::service_block_io();
    process::wake_ready_sleepers(timekeeping::tick_count());
    if ((frame.cs & 0x3) != 0) {
        scheduler::reschedule_from_interrupt(frame);
        return;
    }
    // If we interrupted kernel mode, avoid clobbering the kernel frame; the
    // async I/O worker still makes progress above.
}

[[noreturn]] void run_cpu() {
    asm volatile("sti");
    run_loop();
}

[[noreturn]] void run() {
    run_cpu();
}

}  // namespace scheduler
