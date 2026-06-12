#include <stddef.h>
#include <stdint.h>
#include <neutrino.h>

int main(uint64_t arg_ptr, uint64_t) {
    const char* args = reinterpret_cast<const char*>(arg_ptr);

    long console = neutrino_open_stdout();

    char source[128];
    char dest[128];
    if (!neutrino_parse_two_args(args, source, sizeof(source), dest, sizeof(dest))) {
        neutrino_write_line(console, "usage: cp <source> <dest>");
        return 1;
    }

    if (!neutrino_copy_file(source, dest)) {
        neutrino_write(console, "cp: unable to copy ");
        neutrino_write(console, source);
        neutrino_write(console, " to ");
        neutrino_write_line(console, dest);
        return 1;
    }
    return 0;
}
