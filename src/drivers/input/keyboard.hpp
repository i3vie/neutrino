#pragma once

#include <stddef.h>
#include <stdint.h>

namespace keyboard {

void init();
void handle_irq();
size_t read(uint32_t slot, char* buffer, size_t max_length);
void inject_char(char ch);

}  // namespace keyboard
