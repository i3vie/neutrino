#pragma once

#include <stddef.h>

namespace string_util {

size_t length(const char* str);
void copy(char* dest, size_t dest_size, const char* src);
bool starts_with(const char* str, const char* prefix);
bool equals(const char* a, const char* b);
bool contains(const char* str, char ch);

}  // namespace string_util

