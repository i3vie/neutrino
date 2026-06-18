#include <stddef.h>
#include <stdint.h>
#include <neutrino.h>
#include "../crt/syscall.hpp"

int main(uint64_t, uint64_t) {
    long console = neutrino_open_stdout();

    char cwd[256];
    if (getcwd(cwd, sizeof(cwd)) < 0) {
        neutrino_write_line(console, "pwd: unable to read current directory");
        return 1;
    }

    neutrino_write_line(console, cwd);
    return 0;
}
