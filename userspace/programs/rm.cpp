#include <stddef.h>
#include <stdint.h>
#include <neutrino.h>
#include "../crt/syscall.hpp"

int main(uint64_t arg_ptr, uint64_t) {
    const char* path = reinterpret_cast<const char*>(arg_ptr);

    long console = neutrino_open_stdout();

    if (path == nullptr || path[0] == '\0') {
        neutrino_write_line(console, "usage: rm <path>");
        return 1;
    }

    if (file_remove(path) == 0 || directory_remove(path) == 0) {
        return 0;
    }

    neutrino_write(console, "rm: unable to remove ");
    neutrino_write_line(console, path);
    return 1;
}
