#include <stddef.h>
#include <stdint.h>
#include <neutrino.h>
#include "../crt/syscall.hpp"

namespace {

constexpr size_t kDefaultLines = 10;

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

}  // namespace

int main(uint64_t arg_ptr, uint64_t) {
    const char* args = reinterpret_cast<const char*>(arg_ptr);
    long console = neutrino_open_stdout();

    char path[128];
    if (!parse_path(args, path, sizeof(path))) {
        neutrino_write_line(console, "usage: head <path>");
        return 1;
    }

    long file = file_open(path);
    if (file < 0) {
        neutrino_write(console, "head: unable to open ");
        neutrino_write_line(console, path);
        return 1;
    }

    uint8_t buffer[256];
    size_t lines = 0;
    bool ok = true;
    while (lines < kDefaultLines) {
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

        size_t write_len = 0;
        for (; write_len < static_cast<size_t>(read); ++write_len) {
            if (buffer[write_len] == '\n') {
                ++lines;
                if (lines >= kDefaultLines) {
                    ++write_len;
                    break;
                }
            }
        }
        descriptor_write(static_cast<uint32_t>(console), buffer, write_len);
    }

    file_close(static_cast<uint32_t>(file));
    if (!ok) {
        neutrino_write_line(console, "head: read error");
        return 1;
    }
    return 0;
}
