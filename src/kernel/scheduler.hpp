#pragma once

#include "arch/x86_64/isr.hpp"
#include "arch/x86_64/syscall.hpp"
#include "arch/x86_64/percpu.hpp"
#include "process.hpp"

namespace scheduler {

void init();
void enqueue(process::Process* proc);
[[noreturn]] void run();
[[noreturn]] void run_cpu();
void reschedule(syscall::SyscallFrame& frame);
void reschedule_from_interrupt(InterruptFrame& frame);
process::Process* current();
void register_cpu(percpu::Cpu* cpu);
void tick(InterruptFrame& frame);
size_t cpu_total();

}  // namespace scheduler
