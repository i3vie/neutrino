#pragma once

#include <stdint.h>

namespace work {

using Handler = void (*)(void* context);

// Safe to call from interrupt context. Work runs later on CPU 0.
bool schedule(Handler handler, void* context);
bool busy();
void wait();

}  // namespace work
