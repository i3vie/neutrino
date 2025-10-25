#pragma once

#include <stdint.h>

namespace pic {

void init();
void send_eoi(uint8_t irq);
void set_mask(uint8_t irq, bool masked);

}  // namespace pic

