#include <stddef.h>
#include <stdint.h>
#include <neutrino.h>

int main(uint64_t arg_ptr, uint64_t) {
    const char* text = reinterpret_cast<const char*>(arg_ptr);
    long console = neutrino_open_stdout();

    if (text != nullptr && text[0] != '\0') {
        neutrino_write(console, text);
    }
    neutrino_write(console, "\n");
    return 0;
}
