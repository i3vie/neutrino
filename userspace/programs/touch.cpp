#include <stddef.h>
#include <stdint.h>
#include <neutrino.h>
#include "../crt/syscall.hpp"

int main(uint64_t arg_ptr, uint64_t) {
    const char* path = reinterpret_cast<const char*>(arg_ptr);
    long console = neutrino_open_stdout();

    if (path == nullptr || path[0] == '\0') {
        neutrino_write_line(console, "usage: touch <path>");
        return 1;
    }

    long handle = file_open(path);
    if (handle < 0) {
        handle = file_create(path);
    }
    if (handle < 0) {
        neutrino_write(console, "touch: unable to touch ");
        neutrino_write_line(console, path);
        return 1;
    }

    file_close(static_cast<uint32_t>(handle));
    return 0;
}
