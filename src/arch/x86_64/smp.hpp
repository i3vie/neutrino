#pragma once

#include <stddef.h>

namespace smp {

// Request additional CPUs from Limine and park them in a safe idle loop.
void init();

size_t cpu_count();
size_t online_cpus();

}  // namespace smp
