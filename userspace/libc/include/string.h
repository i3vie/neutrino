#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void* memcpy(void* dest, const void* src, size_t count);
void* memmove(void* dest, const void* src, size_t count);
int memcmp(const void* lhs, const void* rhs, size_t count);
int strcmp(const char* lhs, const char* rhs);
int strncmp(const char* lhs, const char* rhs, size_t count);
size_t strlen(const char* text);
size_t strlcpy(char* dest, const char* src, size_t size);

#ifdef __cplusplus
}
#endif
