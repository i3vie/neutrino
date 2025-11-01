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

void uint64_to_string(uint64_t value, char* buffer, size_t buffer_size) {
    if (buffer == nullptr || buffer_size == 0) {
        return;
    }
    char temp[32];
    size_t pos = 0;
    if (value == 0) {
        temp[pos++] = '0';
    } else {
        while (value > 0 && pos < sizeof(temp)) {
            temp[pos++] = static_cast<char>('0' + (value % 10));
            value /= 10;
        }
    }
    size_t idx = 0;
    while (idx + 1 < buffer_size && pos > 0) {
        buffer[idx++] = temp[--pos];
    }
    buffer[idx] = '\0';
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
        path = ".";
    }

    long console = descriptor_open(DESC_TYPE_CONSOLE, 0);
    if (console < 0) {
        return 1;
    }

    long dir_handle = directory_open(path);
    if (dir_handle < 0) {
        write_console(console, "ls: unable to open ");
        write_line(console, path);
        return 1;
    }

    DirEntry entry{};
    bool had_error = false;
    while (true) {
        long res =
            directory_read(static_cast<uint32_t>(dir_handle), &entry);
        if (res < 0) {
            write_line(console, "ls: error reading directory");
            had_error = true;
            break;
        }
        if (res == 0) {
            break;
        }

        write_console(console, entry.name);
        if ((entry.flags & DIR_ENTRY_FLAG_DIRECTORY) != 0) {
            write_console(console, "/");
        } else {
            write_console(console, " ");
            char size_buffer[32];
            uint64_to_string(entry.size, size_buffer, sizeof(size_buffer));
            write_console(console, size_buffer);
        }
        descriptor_write(static_cast<uint32_t>(console), "\n", 1);
    }

    directory_close(static_cast<uint32_t>(dir_handle));
    return had_error ? 1 : 0;
}
