#include <stddef.h>
#include <stdint.h>
#include <neutrino.h>
#include "../crt/syscall.hpp"

int main(uint64_t arg_ptr, uint64_t) {
    const char* path = reinterpret_cast<const char*>(arg_ptr);

    long console = neutrino_open_stdout();

    if (path == nullptr || path[0] == '\0') {
        neutrino_write_line(console, "usage: mkdir <path>");
        return 1;
    }

    if (directory_create(path) < 0) {
        neutrino_write(console, "mkdir: unable to create ");
        neutrino_write_line(console, path);
        return 1;
    }
    return 0;
}
