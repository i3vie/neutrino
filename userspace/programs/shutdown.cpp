#include <stddef.h>
#include <stdint.h>

#include <neutrino.h>

int main(uint64_t, uint64_t) {
    long console = neutrino_open_stdout();
    if (!neutrino_shutdown()) {
        neutrino_write_line(console, "shutdown: failed");
    }
    return 1;
}
