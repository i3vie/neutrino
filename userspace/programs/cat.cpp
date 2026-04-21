#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "descriptors.hpp"
#include "../crt/syscall.hpp"

constexpr uint32_t kDescConsole =
    static_cast<uint32_t>(descriptor_defs::Type::Console);

namespace {

void write_console(long console, const char* text) {
    if (console < 0 || text == nullptr) {
        return;
    }
    descriptor_write(static_cast<uint32_t>(console),
                     text,
                     strlen(text));
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

    long console = process_get_standard_descriptor(1);
    if (console < 0) {
        console = descriptor_open(kDescConsole, 0);
    }
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
