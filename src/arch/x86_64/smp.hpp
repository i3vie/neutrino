#pragma once

#include <stddef.h>

namespace smp {

// Request additional CPUs from Limine and park them in a safe idle loop.
void init();
// Allows parked APs to enter the scheduler after global boot setup is complete.
void release_aps();

size_t cpu_count();
size_t online_cpus();

}  // namespace smp
