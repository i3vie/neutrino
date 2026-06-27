#pragma once

#include <stddef.h>

namespace userspace::text {

bool starts_with(const char* text, const char* prefix);
bool append_char(char* out, size_t out_size, size_t& len, char ch);
bool append_text(char* out, size_t out_size, size_t& len, const char* text);

}  // namespace userspace::text
