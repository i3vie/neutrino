#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "descriptors.hpp"
#include "../crt/syscall.hpp"

namespace {

constexpr uint32_t kDescConsole =
    static_cast<uint32_t>(descriptor_defs::Type::Console);
constexpr uint32_t kDescKernelLog =
    static_cast<uint32_t>(descriptor_defs::Type::KernelLog);

bool contains(const char* text, const char* needle) {
    if (needle == nullptr || *needle == '\0') return true;
    size_t needle_length = strlen(needle);
    for (size_t i = 0; text[i] != '\0'; ++i) {
        if (strncmp(text + i, needle, needle_length) == 0) return true;
    }
    return false;
}

const char* parse_filter(const char* args, bool& valid) {
    const char* cursor = args == nullptr ? "" : args;
    while (*cursor == ' ' || *cursor == '\t' ||
           *cursor == '\r' || *cursor == '\n') ++cursor;
    if (*cursor == '\0') return nullptr;

    const char* end = cursor;
    while (*end != '\0' && *end != ' ' && *end != '\t' &&
           *end != '\r' && *end != '\n') ++end;
    const char* rest = end;
    while (*rest == ' ' || *rest == '\t' ||
           *rest == '\r' || *rest == '\n') ++rest;
    valid = *rest == '\0';
    return cursor;
}

}  // namespace

int main(uint64_t arg_ptr, uint64_t) {
    long console = process_get_standard_descriptor(1);
    if (console < 0) console = descriptor_open(kDescConsole, 0);

    bool valid = true;
    const char* raw_filter = parse_filter(
        reinterpret_cast<const char*>(arg_ptr), valid);
    if (!valid) {
        descriptor_write(static_cast<uint32_t>(console),
                         "usage: dmesg [filter]\n", 22);
        return 1;
    }

    char filter[64]{};
    if (raw_filter != nullptr) {
        size_t length = 0;
        while (raw_filter[length] != '\0' && raw_filter[length] != ' ' &&
               raw_filter[length] != '\t' && raw_filter[length] != '\r' &&
               raw_filter[length] != '\n') {
            if (length + 1 >= sizeof(filter)) {
                descriptor_write(static_cast<uint32_t>(console),
                                 "dmesg: filter too long\n", 23);
                return 1;
            }
            filter[length] = raw_filter[length];
            ++length;
        }
        filter[length] = '\0';
    }

    long log = descriptor_open(kDescKernelLog, 0);
    if (log < 0) {
        descriptor_write(static_cast<uint32_t>(console),
                         "dmesg: kernel log unavailable\n", 30);
        return 1;
    }

    char input[512];
    char line[512];
    size_t line_length = 0;
    uint64_t offset = 0;
    bool matched = false;
    for (;;) {
        long bytes = descriptor_read(static_cast<uint32_t>(log),
                                     input, sizeof(input), offset);
        if (bytes < 0) {
            descriptor_close(static_cast<uint32_t>(log));
            return 1;
        }
        if (bytes == 0) break;
        offset += static_cast<uint64_t>(bytes);
        for (long i = 0; i < bytes; ++i) {
            char ch = input[i];
            if (ch != '\n' && line_length + 1 < sizeof(line)) {
                line[line_length++] = ch;
            }
            if (ch == '\n' || line_length + 1 == sizeof(line)) {
                line[line_length] = '\0';
                if (contains(line, raw_filter == nullptr ? nullptr : filter)) {
                    descriptor_write(static_cast<uint32_t>(console),
                                     line, line_length);
                    descriptor_write(static_cast<uint32_t>(console), "\n", 1);
                    matched = true;
                }
                line_length = 0;
            }
        }
    }
    if (line_length != 0) {
        line[line_length] = '\0';
        if (contains(line, raw_filter == nullptr ? nullptr : filter)) {
            descriptor_write(static_cast<uint32_t>(console), line, line_length);
            descriptor_write(static_cast<uint32_t>(console), "\n", 1);
            matched = true;
        }
    }
    descriptor_close(static_cast<uint32_t>(log));
    return raw_filter != nullptr && !matched ? 1 : 0;
}
