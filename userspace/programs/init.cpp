#include <stddef.h>
#include <stdint.h>

#include "../crt/syscall.hpp"

namespace {

constexpr const char* kDefaultShellPath = "binary/shell.elf";
constexpr const char* kSpawnConfigPath = "config/spawn.cfg";
constexpr size_t kConfigBufferSize = 1024;
constexpr size_t kMaxPathLength = 160;

uint32_t g_console_handle = kInvalidDescriptor;

void print(const char* text) {
    if (g_console_handle == kInvalidDescriptor || text == nullptr) {
        return;
    }
    size_t length = 0;
    while (text[length] != '\0') {
        ++length;
    }
    if (length != 0) {
        descriptor_write(g_console_handle, text, length);
    }
}

void print_line(const char* text) {
    print(text);
    print("\n");
}

bool is_space(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
}

char* skip_spaces(char* text) {
    if (text == nullptr) {
        return nullptr;
    }
    while (*text == ' ' || *text == '\t') {
        ++text;
    }
    return text;
}

void trim_trailing(char* start, char* end) {
    if (start == nullptr || end == nullptr || end < start) {
        return;
    }
    while (end > start) {
        char ch = *(end - 1);
        if (ch != ' ' && ch != '\t') {
            break;
        }
        --end;
    }
    *end = '\0';
}

bool build_mount_subpath(const char* mount,
                         const char* suffix,
                         char* out,
                         size_t out_size) {
    if (mount == nullptr || mount[0] == '\0' ||
        out == nullptr || out_size == 0) {
        return false;
    }

    size_t idx = 0;
    out[idx++] = '/';
    for (size_t i = 0; mount[i] != '\0'; ++i) {
        if (idx + 1 >= out_size) {
            return false;
        }
        out[idx++] = mount[i];
    }

    if (suffix != nullptr && suffix[0] != '\0') {
        if (idx + 1 >= out_size) {
            return false;
        }
        out[idx++] = '/';
        for (size_t i = 0; suffix[i] != '\0'; ++i) {
            if (idx + 1 >= out_size) {
                return false;
            }
            out[idx++] = suffix[i];
        }
    }

    out[idx] = '\0';
    return true;
}

bool read_file_into_buffer(const char* path,
                           char* buffer,
                           size_t buffer_size,
                           size_t& out_len) {
    out_len = 0;
    if (path == nullptr || buffer == nullptr || buffer_size == 0) {
        return false;
    }

    long handle = file_open(path);
    if (handle < 0) {
        return false;
    }

    size_t total = 0;
    while (total + 1 < buffer_size) {
        long read = file_read(static_cast<uint32_t>(handle),
                              buffer + total,
                              buffer_size - 1 - total);
        if (read <= 0) {
            break;
        }
        total += static_cast<size_t>(read);
    }

    file_close(static_cast<uint32_t>(handle));
    buffer[total] = '\0';
    out_len = total;
    return total > 0;
}

bool read_file_from_mounts(const char* suffix,
                           char* buffer,
                           size_t buffer_size,
                           size_t& out_len) {
    if (read_file_into_buffer(suffix, buffer, buffer_size, out_len)) {
        return true;
    }

    long dir = directory_open("/");
    if (dir < 0) {
        return false;
    }

    DirEntry entry{};
    char path[kMaxPathLength];
    while (directory_read(static_cast<uint32_t>(dir), &entry) > 0) {
        if (entry.name[0] == '\0') {
            continue;
        }
        if (!build_mount_subpath(entry.name, suffix, path, sizeof(path))) {
            continue;
        }
        if (read_file_into_buffer(path, buffer, buffer_size, out_len)) {
            directory_close(static_cast<uint32_t>(dir));
            return true;
        }
    }

    directory_close(static_cast<uint32_t>(dir));
    return false;
}

bool spawn_command_line(char* line) {
    if (line == nullptr || line[0] == '\0') {
        return false;
    }

    char* command = skip_spaces(line);
    if (command == nullptr || command[0] == '\0' || command[0] == '#') {
        return false;
    }

    char* cursor = command;
    while (*cursor != '\0' && !is_space(*cursor)) {
        ++cursor;
    }

    char* args = nullptr;
    if (*cursor != '\0') {
        *cursor++ = '\0';
        args = skip_spaces(cursor);
        if (args != nullptr && args[0] == '\0') {
            args = nullptr;
        }
    }

    long pid = child(command, args, 0, nullptr);
    if (pid >= 0) {
        print("init: spawned ");
        print(command);
        print("\n");
        return true;
    }

    long dir = directory_open("/");
    if (dir < 0) {
        return false;
    }

    DirEntry entry{};
    char path[kMaxPathLength];
    while (directory_read(static_cast<uint32_t>(dir), &entry) > 0) {
        if (entry.name[0] == '\0') {
            continue;
        }
        if (!build_mount_subpath(entry.name, command, path, sizeof(path))) {
            continue;
        }
        pid = child(path, args, 0, nullptr);
        if (pid >= 0) {
            directory_close(static_cast<uint32_t>(dir));
            print("init: spawned ");
            print(path);
            print("\n");
            return true;
        }
    }

    directory_close(static_cast<uint32_t>(dir));
    print("init: failed to spawn ");
    print(command);
    print("\n");
    return false;
}

bool exec_command_line(char* line) {
    if (line == nullptr || line[0] == '\0') {
        return false;
    }

    char* command = skip_spaces(line);
    if (command == nullptr || command[0] == '\0' || command[0] == '#') {
        return false;
    }

    char* cursor = command;
    while (*cursor != '\0' && !is_space(*cursor)) {
        ++cursor;
    }

    char* args = nullptr;
    if (*cursor != '\0') {
        *cursor++ = '\0';
        args = skip_spaces(cursor);
        if (args != nullptr && args[0] == '\0') {
            args = nullptr;
        }
    }

    long result = exec(command, args, 0, nullptr);
    if (result >= 0) {
        return true;
    }

    long dir = directory_open("/");
    if (dir < 0) {
        return false;
    }

    DirEntry entry{};
    char path[kMaxPathLength];
    while (directory_read(static_cast<uint32_t>(dir), &entry) > 0) {
        if (entry.name[0] == '\0') {
            continue;
        }
        if (!build_mount_subpath(entry.name, command, path, sizeof(path))) {
            continue;
        }
        result = exec(path, args, 0, nullptr);
        if (result >= 0) {
            directory_close(static_cast<uint32_t>(dir));
            return true;
        }
    }

    directory_close(static_cast<uint32_t>(dir));
    print("init: failed to exec ");
    print(command);
    print("\n");
    return false;
}

bool spawn_from_config() {
    char buffer[kConfigBufferSize];
    size_t len = 0;
    if (!read_file_from_mounts(kSpawnConfigPath, buffer, sizeof(buffer), len)) {
        return false;
    }

    char* lines[32];
    size_t line_count = 0;
    char* cursor = buffer;
    while (cursor != nullptr && *cursor != '\0' && line_count < 32) {
        char* line_start = cursor;
        while (*cursor != '\0' && *cursor != '\n' && *cursor != '\r') {
            ++cursor;
        }
        char* line_end = cursor;
        while (*cursor == '\n' || *cursor == '\r') {
            *cursor = '\0';
            ++cursor;
        }

        char* trimmed = skip_spaces(line_start);
        trim_trailing(trimmed, line_end);
        if (trimmed == nullptr || trimmed[0] == '\0' || trimmed[0] == '#') {
            continue;
        }
        lines[line_count++] = trimmed;
    }

    if (line_count == 0) {
        return false;
    }

    bool spawned_any = false;
    for (size_t i = 0; i + 1 < line_count; ++i) {
        if (spawn_command_line(lines[i])) {
            spawned_any = true;
        }
    }

    if (exec_command_line(lines[line_count - 1])) {
        return true;
    }

    return spawned_any;
}

[[noreturn]] void idle_loop() {
    for (;;) {
        yield();
    }
}

}  // namespace

int main(uint64_t, uint64_t) {
    long console = descriptor_open(
        static_cast<uint32_t>(descriptor_defs::Type::Console), 0);
    if (console >= 0) {
        g_console_handle = static_cast<uint32_t>(console);
    }

    if (!spawn_from_config()) {
        (void)exec(kDefaultShellPath, nullptr, 0, nullptr);
    }
    idle_loop();
}
