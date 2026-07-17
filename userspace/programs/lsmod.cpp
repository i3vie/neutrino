#include <stddef.h>
#include <stdint.h>

#include "../crt/syscall.hpp"
#include "../helpers/console.hpp"

namespace {

void print_size(long console, uint64_t bytes) {
    constexpr uint64_t kib = 1024ull;
    if (bytes >= kib) {
        userspace::write_u64(console, bytes / kib);
        userspace::write(console, " KiB");
    } else {
        userspace::write_u64(console, bytes);
        userspace::write(console, " B");
    }
}

}  // namespace

int main(uint64_t, uint64_t) {
    long console = process_get_standard_descriptor(1);
    if (console < 0) {
        console = descriptor_open(
            static_cast<uint32_t>(descriptor_defs::Type::Console));
    }

    long count = module_count();
    if (count < 0) {
        userspace::write_line(console, "lsmod: unable to read module list");
        return 1;
    }

    userspace::write_line(console, "name                 type     size    path");
    for (long i = 0; i < count; ++i) {
        ModuleInfo info{};
        if (module_info(static_cast<size_t>(i), &info) != 0) {
            continue;
        }
        userspace::write(console, info.name[0] != '\0' ? info.name : "(unnamed)");
        size_t len = 0;
        while (info.name[len] != '\0' && len < sizeof(info.name)) {
            ++len;
        }
        while (len < 21) {
            userspace::write(console, " ");
            ++len;
        }

        const char* type =
            (info.flags & kModuleInfoDynamic) != 0 ? "dynamic" : "builtin";
        userspace::write(console, type);
        size_t type_len = 0;
        while (type[type_len] != '\0') {
            ++type_len;
        }
        while (type_len < 9) {
            userspace::write(console, " ");
            ++type_len;
        }

        print_size(console, info.image_size);
        userspace::write(console, "  ");
        if (info.path[0] != '\0') {
            userspace::write(console, info.path);
        }
        userspace::write(console, "\n");
    }

    return 0;
}
