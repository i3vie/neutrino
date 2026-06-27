#pragma once

#include <stddef.h>

namespace userspace {

bool is_space(char ch);
const char* skip_spaces(const char* text);
bool copy_token(const char*& cursor, char* out, size_t out_size);
bool only_spaces_remain(const char* text);

}  // namespace userspace
