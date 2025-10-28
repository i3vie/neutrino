#pragma once

#include <stddef.h>
#include <stdint.h>

namespace keyboard {

void init();
void handle_irq();
size_t read(char* buffer, size_t max_length);

}  // namespace keyboard

