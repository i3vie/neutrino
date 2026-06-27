#include <stddef.h>
#include <stdint.h>

#include <neutrino.h>

int main(uint64_t, uint64_t) {
    long console = neutrino_open_stdout();
    if (!neutrino_sync()) {
        neutrino_write_line(console, "sync: failed");
        return 1;
    }
    return 0;
}
