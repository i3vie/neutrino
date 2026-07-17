#include <stddef.h>
#include <stdint.h>

#include "../crt/syscall.hpp"
#include "../helpers/args.hpp"
#include "../helpers/console.hpp"

int main(uint64_t arg_ptr, uint64_t) {
    long console = process_get_standard_descriptor(1);
    if (console < 0) {
        console = descriptor_open(
            static_cast<uint32_t>(descriptor_defs::Type::Console));
    }

    const char* args = reinterpret_cast<const char*>(arg_ptr);
    char path[128];
    const char* cursor = args;
    if (!userspace::copy_token(cursor, path, sizeof(path)) ||
        !userspace::only_spaces_remain(cursor)) {
        userspace::write_line(console, "usage: insmod <module.ko>");
        return 1;
    }

    if (module_load(path) != 0) {
        userspace::write(console, "insmod: failed to load ");
        userspace::write_line(console, path);
        return 1;
    }

    return 0;
}
