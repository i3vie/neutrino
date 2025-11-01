#include "scheduler.hpp"

#include "drivers/log/logging.hpp"
#include "lib/mem.hpp"
#include "arch/x86_64/gdt.hpp"
#include "arch/x86_64/tss.hpp"
#include "userspace.hpp"

namespace {

process::Process* g_ready_queue[process::kMaxProcesses];
size_t g_ready_head = 0;
size_t g_ready_count = 0;

void halt_system() {
    for (;;) {
        asm volatile("hlt");
    }
}

bool queue_contains(process::Process* proc) {
    size_t idx = g_ready_head;
    for (size_t i = 0; i < g_ready_count; ++i) {
        if (g_ready_queue[idx] == proc) {
            return true;
        }
        idx = (idx + 1) % process::kMaxProcesses;
    }
    return false;
}

void queue_push(process::Process* proc) {
    if (g_ready_count >= process::kMaxProcesses) {
        return;
    }
    size_t tail = (g_ready_head + g_ready_count) % process::kMaxProcesses;
    g_ready_queue[tail] = proc;
    ++g_ready_count;
}

process::Process* queue_pop() {
    if (g_ready_count == 0) {
        return nullptr;
    }
    process::Process* proc = g_ready_queue[g_ready_head];
    g_ready_head = (g_ready_head + 1) % process::kMaxProcesses;
    --g_ready_count;
    return proc;
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
    g_ready_head = 0;
    g_ready_count = 0;
}

process::Process* current() {
    return process::current();
}

void enqueue(process::Process* proc) {
    if (proc == nullptr) {
        return;
    }
    if (proc->state == process::State::Terminated) {
        return;
    }
    if (!queue_contains(proc)) {
        queue_push(proc);
    }
    proc->state = process::State::Ready;
}

[[noreturn]] void run() {
    for (;;) {
        process::Process* next = queue_pop();
        if (next == nullptr) {
            log_message(LogLevel::Error,
                        "Scheduler: no runnable processes, halting");
            halt_system();
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
    } else {
        current_proc->has_context = false;
    }

    if (!terminated) {
        if (current_proc->state == process::State::Running) {
            current_proc->state = process::State::Ready;
        }
        if (current_proc->state == process::State::Ready) {
            enqueue(current_proc);
        }
    }

    process::Process* next = queue_pop();
    while (next != nullptr && next->state == process::State::Terminated) {
        next = queue_pop();
    }

    if (next == nullptr) {
        if (terminated) {
            log_message(LogLevel::Error,
                        "Scheduler: no runnable processes after termination, halting");
            halt_system();
        }
        process::set_current(current_proc);
        prepare_frame_for_process(*current_proc, frame);
        return;
    }

    process::set_current(next);
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

}  // namespace scheduler
