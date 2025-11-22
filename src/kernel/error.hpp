#pragma once

#include "arch/x86_64/isr.hpp"

namespace error_screen {
[[noreturn]] void display(const char* primary,
                         const char* secondary,
                         const InterruptFrame* regs);
}

