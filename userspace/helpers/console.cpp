#include "console.hpp"

#include <stddef.h>
#include <string.h>

#include "../crt/syscall.hpp"

namespace userspace {

void write(long console, const char* text) {
    if (console < 0 || text == nullptr) {
        return;
    }
    descriptor_write(static_cast<uint32_t>(console), text, strlen(text));
}

void write_line(long console, const char* text) {
    write(console, text);
    descriptor_write(static_cast<uint32_t>(console), "\n", 1);
}

void write_u64(long console, uint64_t value) {
    char buffer[32];
    size_t pos = sizeof(buffer);
    buffer[--pos] = '\0';
    if (value == 0) {
        buffer[--pos] = '0';
    } else {
        while (value != 0 && pos > 0) {
            buffer[--pos] = static_cast<char>('0' + (value % 10));
            value /= 10;
        }
    }
    write(console, &buffer[pos]);
}

}  // namespace userspace
