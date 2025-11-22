#pragma once

#include <stdint.h>

namespace lapic {

void init(uint64_t hhdm_offset);
void setup_timer(uint8_t vector, uint32_t initial_count);
void eoi();
void send_ipi_all_others(uint8_t vector);
uint32_t id();

}  // namespace lapic
