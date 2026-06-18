#include <stddef.h>
#include <stdint.h>
#include <neutrino.h>
#include "../crt/syscall.hpp"

namespace {

bool is_space(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' ||
           ch == '\v' || ch == '\f';
}

void u64_to_dec(uint64_t value, char* out, size_t out_size) {
    if (out == nullptr || out_size == 0) {
        return;
    }
    char temp[32];
    size_t temp_len = 0;
    if (value == 0) {
        temp[temp_len++] = '0';
    } else {
        while (value > 0 && temp_len < sizeof(temp)) {
            temp[temp_len++] = static_cast<char>('0' + (value % 10u));
            value /= 10u;
        }
    }

    size_t len = temp_len < out_size - 1 ? temp_len : out_size - 1;
    for (size_t i = 0; i < len; ++i) {
        out[i] = temp[temp_len - 1u - i];
    }
    out[len] = '\0';
}

bool parse_path(const char* args, char* path, size_t path_size) {
    if (args == nullptr || path == nullptr || path_size == 0) {
        return false;
    }
    while (*args == ' ' || *args == '\t') {
        ++args;
    }
    if (*args == '\0') {
        return false;
    }
    size_t len = 0;
    while (args[len] != '\0' && args[len] != ' ' && args[len] != '\t') {
        if (len + 1 >= path_size) {
            return false;
        }
        path[len] = args[len];
        ++len;
    }
    path[len] = '\0';
    return true;
}

void write_count(long console, uint64_t value) {
    char buffer[32];
    u64_to_dec(value, buffer, sizeof(buffer));
    neutrino_write(console, buffer);
}

}  // namespace

int main(uint64_t arg_ptr, uint64_t) {
    const char* args = reinterpret_cast<const char*>(arg_ptr);
    long console = neutrino_open_stdout();

    char path[128];
    if (!parse_path(args, path, sizeof(path))) {
        neutrino_write_line(console, "usage: wc <path>");
        return 1;
    }

    long file = file_open(path);
    if (file < 0) {
        neutrino_write(console, "wc: unable to open ");
        neutrino_write_line(console, path);
        return 1;
    }

    uint64_t lines = 0;
    uint64_t words = 0;
    uint64_t bytes = 0;
    bool in_word = false;
    bool ok = true;
    uint8_t buffer[512];

    while (true) {
        long read = file_read(static_cast<uint32_t>(file),
                              buffer,
                              sizeof(buffer));
        if (read < 0) {
            ok = false;
            break;
        }
        if (read == 0) {
            break;
        }

        bytes += static_cast<uint64_t>(read);
        for (size_t i = 0; i < static_cast<size_t>(read); ++i) {
            char ch = static_cast<char>(buffer[i]);
            if (ch == '\n') {
                ++lines;
            }
            if (is_space(ch)) {
                in_word = false;
            } else if (!in_word) {
                in_word = true;
                ++words;
            }
        }
    }

    file_close(static_cast<uint32_t>(file));
    if (!ok) {
        neutrino_write_line(console, "wc: read error");
        return 1;
    }

    write_count(console, lines);
    neutrino_write(console, " ");
    write_count(console, words);
    neutrino_write(console, " ");
    write_count(console, bytes);
    neutrino_write(console, " ");
    neutrino_write_line(console, path);
    return 0;
}
