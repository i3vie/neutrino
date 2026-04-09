#pragma once

#include <stdint.h>

namespace interrupts {

using VectorHandler = void (*)();

uint8_t allocate_vector();
void free_vector(uint8_t vector);
bool register_vector(uint8_t vector, VectorHandler handler);
void unregister_vector(uint8_t vector);
bool dispatch(uint8_t vector);

}  // namespace interrupts
