#include "../crt/syscall.hpp"
#include <stddef.h>
#include <stdint.h>

#define DESC_TYPE_CONSOLE 1

namespace {

size_t string_length(const char* str) {
    if (str == nullptr) {
        return 0;
    }
    size_t len = 0;
    while (str[len] != '\0') {
        ++len;
    }
    return len;
}

void write_console(long console, const char* text) {
    if (console < 0 || text == nullptr) {
        return;
    }
    descriptor_write(static_cast<uint32_t>(console),
                     text,
                     string_length(text));
}

void write_line(long console, const char* text) {
    write_console(console, text);
    descriptor_write(static_cast<uint32_t>(console), "\n", 1);
}

}  // namespace

int main(uint64_t arg_ptr, uint64_t /*flags*/) {
    const char* path = reinterpret_cast<const char*>(arg_ptr);
    if (path == nullptr || path[0] == '\0') {
        return 1;
    }

    long console = descriptor_open(DESC_TYPE_CONSOLE, 0);
    if (console < 0) {
        return 1;
    }

    long file_handle = file_open(path);
    if (file_handle < 0) {
        write_console(console, "cat: unable to open ");
        write_line(console, path);
        return 1;
    }

    uint8_t buffer[256];
    bool had_error = false;
    while (true) {
        long res = file_read(static_cast<uint32_t>(file_handle),
                             buffer,
                             sizeof(buffer));
        if (res < 0) {
            write_line(console, "cat: error reading file");
            had_error = true;
            break;
        }
        if (res == 0) {
            break;
        }
        descriptor_write(static_cast<uint32_t>(console),
                         buffer,
                         static_cast<size_t>(res));
    }

    descriptor_write(static_cast<uint32_t>(console), "\n", 1);
    file_close(static_cast<uint32_t>(file_handle));
    return had_error ? 1 : 0;
}
