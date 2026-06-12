#include "neutrino.h"

#include <ctype.h>
#include <stdint.h>
#include <string.h>

#include "descriptors.hpp"
#include "syscall.hpp"

constexpr uint32_t kDescConsole =
    static_cast<uint32_t>(descriptor_defs::Type::Console);

namespace {

const char* skip_spaces(const char* text) {
    while (text != nullptr && isspace(*text)) {
        ++text;
    }
    return text;
}

bool copy_arg(const char*& cursor, char* out, size_t out_size) {
    if (out == nullptr || out_size == 0) {
        return false;
    }
    cursor = skip_spaces(cursor);
    if (cursor == nullptr || *cursor == '\0') {
        return false;
    }

    size_t len = 0;
    while (cursor[len] != '\0' && !isspace(cursor[len])) {
        if (len + 1 >= out_size) {
            return false;
        }
        out[len] = cursor[len];
        ++len;
    }
    out[len] = '\0';
    cursor += len;
    return true;
}

}  // namespace

extern "C" bool neutrino_parse_two_args(const char* args,
                                        char* first,
                                        size_t first_size,
                                        char* second,
                                        size_t second_size) {
    const char* cursor = args;
    if (!copy_arg(cursor, first, first_size)) {
        return false;
    }
    if (!copy_arg(cursor, second, second_size)) {
        return false;
    }

    cursor = skip_spaces(cursor);
    return cursor == nullptr || *cursor == '\0';
}

extern "C" bool neutrino_copy_file(const char* source, const char* dest) {
    long in = file_open(source);
    if (in < 0) {
        return false;
    }

    long out = file_create(dest);
    if (out < 0) {
        file_close(static_cast<uint32_t>(in));
        return false;
    }

    uint8_t buffer[512];
    bool ok = true;
    while (true) {
        long read = file_read(static_cast<uint32_t>(in),
                              buffer,
                              sizeof(buffer));
        if (read < 0) {
            ok = false;
            break;
        }
        if (read == 0) {
            break;
        }

        size_t written_total = 0;
        while (written_total < static_cast<size_t>(read)) {
            long written = file_write(static_cast<uint32_t>(out),
                                      buffer + written_total,
                                      static_cast<size_t>(read) - written_total);
            if (written <= 0) {
                ok = false;
                break;
            }
            written_total += static_cast<size_t>(written);
        }
        if (!ok) {
            break;
        }
    }

    file_close(static_cast<uint32_t>(in));
    file_close(static_cast<uint32_t>(out));
    if (!ok) {
        file_remove(dest);
    }
    return ok;
}

extern "C" long neutrino_open_stdout() {
    long console = process_get_standard_descriptor(1);
    if (console < 0) {
        console = descriptor_open(kDescConsole, 0);
    }
    return console;
}

extern "C" void neutrino_write(long console, const char* text) {
    if (console < 0 || text == nullptr) {
        return;
    }
    descriptor_write(static_cast<uint32_t>(console), text, strlen(text));
}

extern "C" void neutrino_write_line(long console, const char* text) {
    neutrino_write(console, text);
    descriptor_write(static_cast<uint32_t>(console), "\n", 1);
}

extern "C" bool neutrino_get_time(NeutrinoWallTime* out_time) {
    return time_get(out_time) == 0;
}
