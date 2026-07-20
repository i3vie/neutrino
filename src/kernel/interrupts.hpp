#pragma once

#include <stdint.h>

namespace interrupts {

using VectorHandler = void (*)();
using IrqHandler = void (*)();

uint8_t allocate_vector();
void free_vector(uint8_t vector);
bool register_vector(uint8_t vector, VectorHandler handler);
void unregister_vector(uint8_t vector);
bool dispatch(uint8_t vector);
bool register_isa_irq(uint8_t irq, IrqHandler handler, uint8_t* out_vector = nullptr);
void unregister_isa_irq(uint8_t irq);

}  // namespace interrupts
