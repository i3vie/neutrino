// build with:
// g++ -ffreestanding -fno-builtin -fno-stack-protector -nostdlib -m64 -fPIE -pie -I../crt ../build/crt0.o shell.cpp -o shell.elf

#include "../crt/syscall.hpp"
#include <stddef.h>
#include <stdint.h>

#define DESC_TYPE_CONSOLE  1  // In the future, these would ideally be in
#define DESC_TYPE_KEYBOARD 3  // kernel-provided headers

namespace {

constexpr size_t kMaxInputLength = 256;

size_t string_length(const char* str) {
    size_t len = 0;
    if (str == nullptr) return 0;
    while (str[len] != '\0') {
        ++len;
    }
    return len;
}

void print(long console, const char* str) {
    if (console < 0 || str == nullptr) {
        return;
    }
    descriptor_write(static_cast<uint32_t>(console),
                     str,
                     string_length(str));
}

void print_line(long console, const char* str) {
    print(console, str);
    descriptor_write(static_cast<uint32_t>(console), "\n", 1);
}

bool strings_equal(const char* a, const char* b) {
    if (a == nullptr || b == nullptr) {
        return false;
    }
    while (*a != '\0' && *b != '\0') {
        if (*a != *b) {
            return false;
        }
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

void uint64_to_string(uint64_t value, char* buffer, size_t buffer_size) {
    if (buffer == nullptr || buffer_size == 0) {
        return;
    }
    char temp[32];
    size_t pos = 0;
    if (value == 0) {
        temp[pos++] = '0';
    } else {
        while (value > 0 && pos < sizeof(temp)) {
            temp[pos++] = static_cast<char>('0' + (value % 10));
            value /= 10;
        }
    }

    size_t idx = 0;
    while (idx + 1 < buffer_size && pos > 0) {
        buffer[idx++] = temp[--pos];
    }
    buffer[idx] = '\0';
}

void execute_ls(long console, const char* path) {
    if (path == nullptr || *path == '\0') {
        print_line(console, "usage: ls <path>");
        return;
    }

    long dir_handle = directory_open(path);
    if (dir_handle < 0) {
        print(console, "ls: unable to open ");
        print_line(console, path);
        return;
    }

    DirEntry entry{};
    while (true) {
        long res =
            directory_read(static_cast<uint32_t>(dir_handle), &entry);
        if (res < 0) {
            print_line(console, "ls: error reading directory");
            break;
        }
        if (res == 0) {
            break;
        }

        print(console, entry.name);
        if ((entry.flags & DIR_ENTRY_FLAG_DIRECTORY) != 0) {
            print(console, "/");
        } else {
            print(console, " ");
            char size_buffer[32];
            uint64_to_string(entry.size, size_buffer, sizeof(size_buffer));
            print(console, size_buffer);
        }
        descriptor_write(static_cast<uint32_t>(console), "\n", 1);
    }

    directory_close(static_cast<uint32_t>(dir_handle));
}

void execute_cat(long console, const char* path) {
    if (path == nullptr || *path == '\0') {
        print_line(console, "usage: cat <path>");
        return;
    }

    long file_handle = file_open(path);
    if (file_handle < 0) {
        print(console, "cat: unable to open ");
        print_line(console, path);
        return;
    }

    uint8_t buffer[256];
    while (true) {
        long res = file_read(static_cast<uint32_t>(file_handle),
                             buffer,
                             sizeof(buffer));
        if (res < 0) {
            print_line(console, "cat: error reading file");
            break;
        }
        if (res == 0) {
            break;
        }
        descriptor_write(static_cast<uint32_t>(console),
                         buffer,
                         static_cast<size_t>(res));
    }

    descriptor_write(static_cast<uint32_t>(console), "\n", 1);
    file_close(static_cast<uint32_t>(file_handle));
}

const char* skip_spaces(const char* str) {
    if (str == nullptr) {
        return nullptr;
    }
    while (*str == ' ' || *str == '\t') {
        ++str;
    }
    return str;
}

void copy_argument(char* dest, size_t dest_size, const char* source) {
    if (dest == nullptr || dest_size == 0) {
        return;
    }
    dest[0] = '\0';
    if (source == nullptr) {
        return;
    }

    const char* start = skip_spaces(source);
    size_t length = 0;
    while (start[length] != '\0') {
        ++length;
    }
    while (length > 0) {
        char ch = start[length - 1];
        if (ch == ' ' || ch == '\t') {
            --length;
        } else {
            break;
        }
    }

    if (length >= dest_size) {
        length = dest_size - 1;
    }
    for (size_t i = 0; i < length; ++i) {
        dest[i] = start[i];
    }
    dest[length] = '\0';
}

size_t render_line(long console,
                   const char* prompt,
                   size_t prompt_len,
                   const char* buffer,
                   size_t buffer_len,
                   size_t previous_len) {
    descriptor_write(static_cast<uint32_t>(console), "\r", 1);
    descriptor_write(static_cast<uint32_t>(console), prompt, prompt_len);
    if (buffer_len > 0) {
        descriptor_write(static_cast<uint32_t>(console),
                         buffer,
                         buffer_len);
    }

    size_t current_len = prompt_len + buffer_len;
    if (previous_len > current_len) {
        size_t diff = previous_len - current_len;
        for (size_t i = 0; i < diff; ++i) {
            descriptor_write(static_cast<uint32_t>(console), " ", 1);
        }
        descriptor_write(static_cast<uint32_t>(console), "\r", 1);
        descriptor_write(static_cast<uint32_t>(console), prompt, prompt_len);
        if (buffer_len > 0) {
            descriptor_write(static_cast<uint32_t>(console),
                             buffer,
                             buffer_len);
        }
        current_len = prompt_len + buffer_len;
    }
    return current_len;
}

void execute_command(long console, const char* line) {
    const char* cursor = skip_spaces(line);
    if (cursor == nullptr || *cursor == '\0') {
        return;
    }

    char command[16];
    size_t cmd_len = 0;
    while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t') {
        if (cmd_len + 1 < sizeof(command)) {
            command[cmd_len++] = *cursor;
        }
        ++cursor;
    }
    command[cmd_len] = '\0';

    cursor = skip_spaces(cursor);

    if (strings_equal(command, "ls")) {
        char path[128];
        copy_argument(path, sizeof(path), cursor);
        execute_ls(console, path);
    } else if (strings_equal(command, "cat")) {
        char path[128];
        copy_argument(path, sizeof(path), cursor);
        execute_cat(console, path);
    } else if (strings_equal(command, "write")) {
        // Expected format: write <path> <data...>
        char path[128];
        cursor = skip_spaces(cursor);
        const char* path_start = cursor;
        if (*cursor == '\0') {
            print_line(console, "usage: write <path> <data>");
            return;
        }

        size_t path_len = 0;
        while (cursor[path_len] != '\0' &&
               cursor[path_len] != ' ' &&
               cursor[path_len] != '\t') {
            ++path_len;
        }

        if (path_len == 0 || path_len >= sizeof(path)) {
            print_line(console, "write: invalid path");
            return;
        }

        for (size_t i = 0; i < path_len; ++i) {
            path[i] = path_start[i];
        }
        path[path_len] = '\0';

        cursor = skip_spaces(path_start + path_len);
        if (*cursor == '\0') {
            print_line(console, "usage: write <path> <data>");
            return;
        }

        long handle = file_open(path);
        if (handle < 0) {
            handle = file_create(path);
            if (handle < 0) {
                print(console, "write: unable to open ");
                print_line(console, path);
                return;
            }
        }

        const char* data = cursor;
        size_t data_len = string_length(data);
        if (data_len == 0) {
            file_close(static_cast<uint32_t>(handle));
            print_line(console, "write: no data provided");
            return;
        }

        long written = file_write(static_cast<uint32_t>(handle),
                                  data,
                                  data_len);
        file_close(static_cast<uint32_t>(handle));

        if (written < 0 || static_cast<size_t>(written) != data_len) {
            print_line(console, "write: failed to write data");
            return;
        }

        print_line(console, "write: ok");
    } else if (strings_equal(command, "help")) {
        print_line(console, "available commands:");
        print_line(console, "  ls <path>");
        print_line(console, "  cat <path>");
        print_line(console, "  write <path> <data>");
        print_line(console, "  help");
    } else {
        print(console, "unknown command: ");
        print_line(console, command);
    }
}

}  // namespace

int main(void) {
    long console = descriptor_open(DESC_TYPE_CONSOLE, 0);
    if (console < 0) return 1;

    long keyboard = descriptor_open(DESC_TYPE_KEYBOARD, 0);
    if (keyboard < 0) return 1;

    const char prompt[] = "> ";
    const size_t prompt_len = sizeof(prompt) - 1;
    descriptor_write(static_cast<uint32_t>(console), "\n", 1);
    descriptor_write(static_cast<uint32_t>(console), prompt, prompt_len);

    char input_buffer[kMaxInputLength];
    size_t input_length = 0;
    input_buffer[0] = '\0';
    size_t rendered_length = prompt_len;

    uint8_t key;
    while (1) {
        long r = descriptor_read(static_cast<uint32_t>(keyboard), &key, 1);
        if (r > 0) {
            if (key == '\r' || key == '\n') {
                descriptor_write(static_cast<uint32_t>(console), "\n", 1);
                input_buffer[input_length] = '\0';
                execute_command(console, input_buffer);
                input_length = 0;
                input_buffer[0] = '\0';
                descriptor_write(static_cast<uint32_t>(console),
                                 prompt,
                                 prompt_len);
                rendered_length = prompt_len;
            } else if (key == '\b' || key == 0x7F) {
                if (input_length > 0) {
                    --input_length;
                    input_buffer[input_length] = '\0';
                    rendered_length = render_line(console,
                                                  prompt,
                                                  prompt_len,
                                                  input_buffer,
                                                  input_length,
                                                  rendered_length);
                }
            } else if (key == '\t') {
                // ignore tabs for now
            } else if (key >= 0x20 && key <= 0x7E) {
                if (input_length + 1 < sizeof(input_buffer)) {
                    input_buffer[input_length++] = static_cast<char>(key);
                    input_buffer[input_length] = '\0';
                    rendered_length = render_line(console,
                                                  prompt,
                                                  prompt_len,
                                                  input_buffer,
                                                  input_length,
                                                  rendered_length);
                }
            }
        }
        yield();
    }

    return 0;
}
