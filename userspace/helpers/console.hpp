#pragma once

#include <stdint.h>

namespace userspace {

void write(long console, const char* text);
void write_line(long console, const char* text);
void write_u64(long console, uint64_t value);

}  // namespace userspace
