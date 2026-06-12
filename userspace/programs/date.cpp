#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "descriptors.hpp"
#include "neutrino.h"
#include "../crt/syscall.hpp"

constexpr uint32_t kDescConsole =
    static_cast<uint32_t>(descriptor_defs::Type::Console);

namespace {

void append_char(char* buffer, size_t buffer_size, size_t& index, char ch) {
    if (index + 1 >= buffer_size) {
        return;
    }
    buffer[index++] = ch;
    buffer[index] = '\0';
}

void append_two_digits(char* buffer,
                       size_t buffer_size,
                       size_t& index,
                       uint32_t value) {
    append_char(buffer,
                buffer_size,
                index,
                static_cast<char>('0' + ((value / 10u) % 10u)));
    append_char(buffer,
                buffer_size,
                index,
                static_cast<char>('0' + (value % 10u)));
}

void append_four_digits(char* buffer,
                        size_t buffer_size,
                        size_t& index,
                        uint32_t value) {
    append_char(buffer,
                buffer_size,
                index,
                static_cast<char>('0' + ((value / 1000u) % 10u)));
    append_char(buffer,
                buffer_size,
                index,
                static_cast<char>('0' + ((value / 100u) % 10u)));
    append_char(buffer,
                buffer_size,
                index,
                static_cast<char>('0' + ((value / 10u) % 10u)));
    append_char(buffer,
                buffer_size,
                index,
                static_cast<char>('0' + (value % 10u)));
}

void write_console(long console, const char* text) {
    if (console < 0 || text == nullptr) {
        return;
    }
    descriptor_write(static_cast<uint32_t>(console), text, strlen(text));
}

void write_line(long console, const char* text) {
    write_console(console, text);
    descriptor_write(static_cast<uint32_t>(console), "\n", 1);
}

}  // namespace

int main(uint64_t /*arg_ptr*/, uint64_t /*flags*/) {
    long console = neutrino_open_stdout();
    if (console < 0) {
        console = descriptor_open(kDescConsole, 0);
    }
    if (console < 0) {
        return 1;
    }

    NeutrinoWallTime now{};
    if (!neutrino_get_time(&now)) {
        write_line(console, "date: time unavailable");
        return 1;
    }

    char line[64];
    size_t index = 0;
    line[0] = '\0';
    append_four_digits(line, sizeof(line), index, now.year);
    append_char(line, sizeof(line), index, '-');
    append_two_digits(line, sizeof(line), index, now.month);
    append_char(line, sizeof(line), index, '-');
    append_two_digits(line, sizeof(line), index, now.day);
    append_char(line, sizeof(line), index, ' ');
    append_two_digits(line, sizeof(line), index, now.hour);
    append_char(line, sizeof(line), index, ':');
    append_two_digits(line, sizeof(line), index, now.minute);
    append_char(line, sizeof(line), index, ':');
    append_two_digits(line, sizeof(line), index, now.second);
    append_char(line, sizeof(line), index, '\0');

    write_line(console, line);
    return 0;
}
