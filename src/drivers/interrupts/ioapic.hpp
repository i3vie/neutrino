#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

namespace ioapic {

void init(uint64_t hhdm_offset, uint32_t lapic_id);
bool available();
bool route_isa_irq(uint8_t irq, uint8_t vector);
bool handles_irq(uint8_t irq);

}  // namespace ioapic
