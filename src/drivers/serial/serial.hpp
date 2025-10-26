#pragma once
#include <stddef.h>
#include <stdint.h>

namespace serial {

void init();
void write_char(char c);
void write(const char* data, size_t len);
void write_string(const char* str);
size_t read(char* buffer, size_t len);
bool data_available();

}  // namespace serial
