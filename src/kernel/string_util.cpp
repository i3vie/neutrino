#include "string_util.hpp"

namespace string_util {

size_t length(const char* str) {
    if (str == nullptr) {
        return 0;
    }
    size_t len = 0;
    while (str[len] != '\0') {
        ++len;
    }
    return len;
}

void copy(char* dest, size_t dest_size, const char* src) {
    if (dest == nullptr || dest_size == 0) {
        return;
    }
    if (src == nullptr) {
        dest[0] = '\0';
        return;
    }
    size_t idx = 0;
    while (idx + 1 < dest_size && src[idx] != '\0') {
        dest[idx] = src[idx];
        ++idx;
    }
    dest[idx] = '\0';
}

bool starts_with(const char* str, const char* prefix) {
    if (str == nullptr || prefix == nullptr) {
        return false;
    }
    while (*prefix != '\0') {
        if (*str++ != *prefix++) {
            return false;
        }
    }
    return true;
}

bool equals(const char* a, const char* b) {
    if (a == nullptr || b == nullptr) {
        return false;
    }
    while (*a != '\0' && *b != '\0') {
        if (*a != *b) {
            return false;
        }
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

bool contains(const char* str, char ch) {
    if (str == nullptr) {
        return false;
    }
    while (*str != '\0') {
        if (*str == ch) {
            return true;
        }
        ++str;
    }
    return false;
}

}  // namespace string_util

