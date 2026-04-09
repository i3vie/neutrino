#include "interrupts.hpp"

namespace {

interrupts::VectorHandler g_handlers[256]{};
bool g_reserved[256]{};
constexpr uint8_t kFirstAllocVector = 0x50;
constexpr uint8_t kLastAllocVector = 0xFE;

}  // namespace

namespace interrupts {

uint8_t allocate_vector() {
    for (uint16_t candidate = kFirstAllocVector;
         candidate <= kLastAllocVector;
         ++candidate) {
        uint8_t vector = static_cast<uint8_t>(candidate);
        if (!g_reserved[vector] && g_handlers[vector] == nullptr) {
            g_reserved[vector] = true;
            return vector;
        }
    }
    return 0;
}

void free_vector(uint8_t vector) {
    if (vector < kFirstAllocVector || vector > kLastAllocVector) {
        return;
    }
    g_reserved[vector] = false;
}

bool register_vector(uint8_t vector, VectorHandler handler) {
    if (handler == nullptr || vector < 32) {
        return false;
    }
    if (g_handlers[vector] != nullptr && g_handlers[vector] != handler) {
        return false;
    }
    g_reserved[vector] = true;
    g_handlers[vector] = handler;
    return true;
}

void unregister_vector(uint8_t vector) {
    if (vector < 32) {
        return;
    }
    g_handlers[vector] = nullptr;
    free_vector(vector);
}

bool dispatch(uint8_t vector) {
    VectorHandler handler = g_handlers[vector];
    if (handler == nullptr) {
        return false;
    }
    handler();
    return true;
}

}  // namespace interrupts
