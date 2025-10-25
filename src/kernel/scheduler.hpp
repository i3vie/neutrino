#pragma once

#include "arch/x86_64/isr.hpp"
#include "arch/x86_64/syscall.hpp"
#include "process.hpp"

namespace scheduler {

void init();
void enqueue(process::Process* proc);
[[noreturn]] void run();
void reschedule(syscall::SyscallFrame& frame);
void reschedule_from_interrupt(InterruptFrame& frame);
process::Process* current();

}  // namespace scheduler
