#pragma once

#include <stddef.h>
#include <stdint.h>

#include "../crt/syscall.hpp"

namespace auth {

inline bool secure_random(void* output, size_t length) {
    if (length == 0) {
        return true;
    }
    if (output == nullptr) {
        return false;
    }
    return random_get(output, length) == static_cast<long>(length);
}

}  // namespace auth
