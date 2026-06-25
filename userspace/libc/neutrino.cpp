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
    if (source == nullptr || dest == nullptr || source[0] == '\0' ||
        dest[0] == '\0' || strcmp(source, dest) == 0) {
        return false;
    }

    char destination[256];
    strlcpy(destination, dest, sizeof(destination));
    long dest_dir = directory_open(dest);
    if (dest_dir >= 0) {
        directory_close(static_cast<uint32_t>(dest_dir));
        const char* basename = source + strlen(source);
        while (basename > source && basename[-1] == '/') --basename;
        const char* end = basename;
        while (basename > source && basename[-1] != '/') --basename;
        size_t dir_len = strlen(destination);
        size_t name_len = static_cast<size_t>(end - basename);
        bool separator = dir_len != 0 && destination[dir_len - 1] != '/';
        if (name_len == 0 || dir_len + (separator ? 1 : 0) + name_len + 1 >
                                 sizeof(destination)) {
            return false;
        }
        if (separator) destination[dir_len++] = '/';
        memcpy(destination + dir_len, basename, name_len);
        destination[dir_len + name_len] = '\0';
    }
    if (strcmp(source, destination) == 0) return false;

    long in = file_open(source);
    if (in < 0) {
        return false;
    }

    // Creation is exclusive on both FAT32 and Neufs.  Replace an existing
    // regular destination to provide the usual cp overwrite behavior.
    long existing = file_open(destination);
    if (existing >= 0) {
        file_close(static_cast<uint32_t>(existing));
        if (file_remove(destination) < 0) {
            file_close(static_cast<uint32_t>(in));
            return false;
        }
    }

    long out = file_create(destination);
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
        file_remove(destination);
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
