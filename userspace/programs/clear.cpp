#include <stddef.h>
#include <stdint.h>
#include <neutrino.h>
#include "descriptors.hpp"
#include "../crt/syscall.hpp"

int main(uint64_t, uint64_t) {
    long console = neutrino_open_stdout();
    if (console < 0) {
        return 1;
    }

    if (descriptor_set_property(
            static_cast<uint32_t>(console),
            static_cast<uint32_t>(descriptor_defs::Property::ConsoleClear),
            nullptr,
            0) < 0) {
        neutrino_write_line(console, "clear: unable to clear console");
        return 1;
    }
    return 0;
}
