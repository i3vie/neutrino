// build with:
// g++ -ffreestanding -fno-builtin -fno-stack-protector -nostdlib -m64 -fPIE -pie -I../crt ../build/crt0.o shell.cpp -o shell.elf

#include <stddef.h>
#include <stdint.h>
#include <ctype.h>
#include <string.h>
#include "descriptors.hpp"
#include "keyboard_scancode.hpp"
#include "../crt/syscall.hpp"

constexpr uint32_t kDescConsole =
    static_cast<uint32_t>(descriptor_defs::Type::Console);
constexpr uint32_t kDescKeyboard =
    static_cast<uint32_t>(descriptor_defs::Type::Keyboard);
constexpr uint32_t kDescVty =
    static_cast<uint32_t>(descriptor_defs::Type::Vty);

namespace {

constexpr size_t kMaxInputLength = 256;
constexpr size_t kMaxCommandLength = 128;
constexpr size_t kMaxSearchDirs = 8;
constexpr uint32_t kPromptUserColor = 0xFF8FD0FFu;
constexpr uint32_t kPromptPathColor = 0xFFF2F4F8u;
constexpr uint32_t kPromptBgColor = 0x00000000u;

void print(long console, const char* str) {
    if (console < 0 || str == nullptr) {
        return;
    }
    descriptor_write(static_cast<uint32_t>(console),
                     str,
                     strlen(str));
}

void print_line(long console, const char* str) {
    print(console, str);
    descriptor_write(static_cast<uint32_t>(console), "\n", 1);
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

bool contains_slash(const char* str) {
    if (str == nullptr) {
        return false;
    }
    while (*str != '\0') {
        if (*str == '/') {
            return true;
        }
        ++str;
    }
    return false;
}

bool has_dot(const char* str) {
    if (str == nullptr) {
        return false;
    }
    while (*str != '\0') {
        if (*str == '.') {
            return true;
        }
        ++str;
    }
    return false;
}

uint32_t parse_uint32(const char* text) {
    if (text == nullptr) {
        return 0;
    }
    uint32_t value = 0;
    while (*text >= '0' && *text <= '9') {
        uint32_t digit = static_cast<uint32_t>(*text - '0');
        value = value * 10 + digit;
        ++text;
    }
    return value;
}

uint32_t parse_vty_arg(const char* args) {
    if (args == nullptr || args[0] == '\0') {
        return 0;
    }
    const char* cursor = args;
    if (cursor[0] == 'v' && cursor[1] == 't' &&
        cursor[2] == 'y' && cursor[3] == '=') {
        return parse_uint32(cursor + 4);
    }
    return 0;
}

char g_current_cwd[128] = "/";
char g_session_user[32] = "root";

void strip_control(char* str) {
    if (str == nullptr) {
        return;
    }
    size_t write = 0;
    for (size_t read = 0; str[read] != '\0'; ++read) {
        unsigned char ch = static_cast<unsigned char>(str[read]);
        if (ch >= 0x20 && ch <= 0x7E) {
            str[write++] = str[read];
        }
    }
    str[write] = '\0';
}

size_t build_search_directories(const char* cwd,
                                char (&out)[kMaxSearchDirs][128]) {
    (void)cwd;
    size_t count = 0;

    auto append = [&](const char* path) {
        if (path == nullptr || path[0] == '\0') {
            return;
        }
        if (count >= kMaxSearchDirs) {
            return;
        }
        strlcpy(out[count], path, sizeof(out[count]));
        ++count;
    };

    append(".../binary");
    append(".../BINARY");

    return count;
}

size_t build_prompt(char* buffer, size_t buffer_size) {
    if (buffer == nullptr || buffer_size < 6) {
        return 0;
    }
    size_t user_len = strlen(g_session_user);
    size_t cwd_len = strlen(g_current_cwd);
    size_t needed = user_len + cwd_len + 6;
    if (needed > buffer_size) {
        if (buffer_size <= 6) {
            buffer[0] = '>';
            buffer[1] = ' ';
            buffer[2] = '\0';
            return 2;
        }
        size_t available = buffer_size - 6;
        if (user_len > available) {
            user_len = available;
            cwd_len = 0;
        } else {
            cwd_len = available - user_len;
        }
    }
    size_t idx = 0;
    for (size_t i = 0; i < user_len; ++i) {
        buffer[idx++] = g_session_user[i];
    }
    buffer[idx++] = ' ';
    buffer[idx++] = '|';
    buffer[idx++] = ' ';
    for (size_t i = 0; i < cwd_len; ++i) {
        buffer[idx++] = g_current_cwd[i];
    }
    buffer[idx++] = ' ';
    buffer[idx++] = '>';
    buffer[idx++] = ' ';
    buffer[idx] = '\0';
    return idx;
}

bool set_console_color(long console, uint32_t fg, uint32_t bg) {
    descriptor_defs::ColorPair colors{fg, bg};
    return descriptor_set_property(
               static_cast<uint32_t>(console),
               static_cast<uint32_t>(descriptor_defs::Property::ConsoleColor),
               &colors,
               sizeof(colors)) == 0;
}

void write_prompt(long console, const char* prompt, size_t prompt_len) {
    size_t user_len = strlen(g_session_user);
    size_t prefix_len = user_len + 3;
    if (prefix_len > prompt_len) {
        prefix_len = prompt_len;
    }

    set_console_color(console, kPromptUserColor, kPromptBgColor);
    if (prefix_len > 0) {
        descriptor_write(static_cast<uint32_t>(console), prompt, prefix_len);
    }
    set_console_color(console, kPromptPathColor, kPromptBgColor);
    if (prompt_len > prefix_len) {
        descriptor_write(static_cast<uint32_t>(console),
                         prompt + prefix_len,
                         prompt_len - prefix_len);
    }
}

void parse_session_user(const char* args) {
    if (args == nullptr) {
        return;
    }
    const char* cursor = args;
    while (*cursor != '\0') {
        while (isspace(*cursor)) {
            ++cursor;
        }
        if (*cursor == '\0') {
            break;
        }
        const char* token = cursor;
        while (*cursor != '\0' && !isspace(*cursor)) {
            ++cursor;
        }
        size_t len = static_cast<size_t>(cursor - token);
        if (len > 5 && strncmp(token, "user=", strlen("user=")) == 0) {
            size_t copy_len = len - 5;
            if (copy_len >= sizeof(g_session_user)) {
                copy_len = sizeof(g_session_user) - 1;
            }
            for (size_t i = 0; i < copy_len; ++i) {
                g_session_user[i] = token[5 + i];
            }
            g_session_user[copy_len] = '\0';
            return;
        }
    }
}

void maybe_enter_home_directory() {
    if (g_session_user[0] == '\0') {
        return;
    }

    char home[128];
    size_t idx = 0;
    const char prefix[] = "/user/";
    for (size_t i = 0; prefix[i] != '\0' && idx + 1 < sizeof(home); ++i) {
        home[idx++] = prefix[i];
    }
    for (size_t i = 0; g_session_user[i] != '\0' && idx + 1 < sizeof(home); ++i) {
        home[idx++] = g_session_user[i];
    }
    home[idx] = '\0';

    long dir = directory_open(home);
    if (dir < 0) {
        return;
    }
    directory_close(static_cast<uint32_t>(dir));
    strlcpy(g_current_cwd, home, sizeof(g_current_cwd));
    (void)setcwd(g_current_cwd);
}

bool join_path(char* dest,
               size_t dest_size,
               const char* dir,
               const char* cmd,
               const char* suffix) {
    if (dest == nullptr || dest_size == 0 || dir == nullptr || cmd == nullptr) {
        return false;
    }
    size_t dir_len = strlen(dir);
    size_t cmd_len = strlen(cmd);
    size_t suffix_len = (suffix != nullptr) ? strlen(suffix) : 0;
    bool needs_sep = dir_len > 0 && dir[dir_len - 1] != '/';
    size_t required =
        dir_len + (needs_sep ? 1 : 0) + cmd_len + suffix_len + 1;
    if (required > dest_size) {
        return false;
    }
    size_t idx = 0;
    for (size_t i = 0; i < dir_len; ++i) {
        dest[idx++] = dir[i];
    }
    if (needs_sep) {
        dest[idx++] = '/';
    }
    for (size_t i = 0; i < cmd_len; ++i) {
        dest[idx++] = cmd[i];
    }
    if (suffix_len > 0) {
        for (size_t i = 0; i < suffix_len; ++i) {
            dest[idx++] = suffix[i];
        }
    }
    dest[idx] = '\0';
    return true;
}

long run_with_search(const char* command,
                     const char* cwd,
                     const char* args,
                     uint64_t flags,
                     bool wait,
                     char* resolved_path,
                     size_t resolved_path_size) {
    if (resolved_path != nullptr && resolved_path_size > 0) {
        resolved_path[0] = '\0';
    }

    auto invoke_exec = [wait, cwd](const char* path,
                                  const char* arguments,
                                  uint64_t exec_flags) -> long {
        return wait ? exec(path, arguments, exec_flags, cwd)
                    : child(path, arguments, exec_flags, cwd);
    };

    if (contains_slash(command)) {
        long value = invoke_exec(command, args, flags);
        if (value >= 0 && resolved_path != nullptr) {
            strlcpy(resolved_path, command, resolved_path_size);
        }
        return value;
    }

    char search_dirs[kMaxSearchDirs][128];
    size_t search_dir_count = build_search_directories(cwd, search_dirs);

    char candidate[128];
    for (size_t i = 0; i < search_dir_count; ++i) {
        if (search_dirs[i][0] == '\0') {
            continue;
        }
        if (join_path(candidate,
                      sizeof(candidate),
                      search_dirs[i],
                      command,
                      nullptr)) {
            long value = invoke_exec(candidate, args, flags);
            if (value >= 0) {
                if (resolved_path != nullptr) {
                    strlcpy(resolved_path, candidate, resolved_path_size);
                }
                return value;
            }
        }
        if (!has_dot(command) &&
            join_path(candidate,
                      sizeof(candidate),
                      search_dirs[i],
                      command,
                      ".elf")) {
            long value = invoke_exec(candidate, args, flags);
            if (value >= 0) {
                if (resolved_path != nullptr) {
                    strlcpy(resolved_path, candidate, resolved_path_size);
                }
                return value;
            }
        }
    }

    return -1;
}

void report_exec_result(long console,
                        long result,
                        const char* label) {
    if (result < 0) {
        print(console, "exec failed: ");
        print_line(console, label);
        return;
    }
    if (result == 0) {
        return;
    }

    char digits[20];
    uint64_to_string(static_cast<uint64_t>(result), digits, sizeof(digits));

    char message[64];
    size_t idx = 0;
    size_t label_len = strlen(label);
    if (label_len > 16) {
        label_len = 16;
    }
    for (size_t i = 0; i < label_len && idx + 1 < sizeof(message); ++i) {
        message[idx++] = label[i];
    }
    const char suffix[] = " exit ";
    for (size_t i = 0; i < sizeof(suffix) - 1 && idx + 1 < sizeof(message); ++i) {
        message[idx++] = suffix[i];
    }
    size_t digit_len = strlen(digits);
    for (size_t i = 0; i < digit_len && idx + 1 < sizeof(message); ++i) {
        message[idx++] = digits[i];
    }
    message[idx] = '\0';
    print_line(console, message);
}

const char* skip_spaces(const char* str) {
    if (str == nullptr) {
        return nullptr;
    }
    while (isspace(*str)) {
        ++str;
    }
    return str;
}

size_t render_line(long console,
                   const char* prompt,
                   size_t prompt_len,
                   const char* buffer,
                   size_t buffer_len,
                   size_t previous_len) {
    descriptor_write(static_cast<uint32_t>(console), "\r", 1);
    write_prompt(console, prompt, prompt_len);
    if (buffer_len > 0) {
        set_console_color(console, kPromptPathColor, kPromptBgColor);
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
        write_prompt(console, prompt, prompt_len);
        if (buffer_len > 0) {
            set_console_color(console, kPromptPathColor, kPromptBgColor);
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

    char command[kMaxCommandLength];
   size_t cmd_len = 0;
    while (*cursor != '\0' && !isspace(*cursor)) {
        if (cmd_len + 1 < sizeof(command)) {
            command[cmd_len++] = *cursor;
        }
        ++cursor;
    }
    command[cmd_len] = '\0';
    strip_control(command);
    if (command[0] == '\0') {
        return;
    }

    cursor = skip_spaces(cursor);

    if (strcmp(command, "cd") == 0) {
        const char* target_start = skip_spaces(cursor);
        char target_buf[128];
        size_t target_len = 0;
        if (target_start != nullptr && *target_start != '\0') {
            while (target_start[target_len] != '\0' &&
                   !isspace(target_start[target_len])) {
                if (target_len + 1 >= sizeof(target_buf)) {
                    break;
                }
                target_buf[target_len] = target_start[target_len];
                ++target_len;
            }
        }

        if (target_len == 0) {
            target_buf[0] = '/';
            target_buf[1] = '\0';
        } else {
            target_buf[target_len] = '\0';
        }
        strip_control(target_buf);

        if (setcwd(target_buf) < 0) {
            print(console, "cd: no such directory: ");
            print_line(console, target_buf);
            return;
        }

        long cwd_len = getcwd(g_current_cwd, sizeof(g_current_cwd));
        if (cwd_len < 0 || g_current_cwd[0] == '\0') {
            print_line(console, "cd: failed to update process cwd");
        }
        return;
    } else if (strcmp(command, "spawn") == 0) {
        cursor = skip_spaces(cursor);
        if (cursor == nullptr || *cursor == '\0') {
            print_line(console, "usage: spawn <path> [args]");
            return;
        }

        const char* path_start = cursor;
        size_t path_len = 0;
        while (cursor[path_len] != '\0' && !isspace(cursor[path_len])) {
            ++path_len;
        }

        if (path_len == 0) {
            print_line(console, "spawn: invalid path");
            return;
        }

        char path_buf[128];
        if (path_len >= sizeof(path_buf)) {
            print_line(console, "spawn: path too long");
            return;
        }
        for (size_t i = 0; i < path_len; ++i) {
            path_buf[i] = path_start[i];
        }
        path_buf[path_len] = '\0';

        cursor = skip_spaces(path_start + path_len);
        const char* args = (*cursor != '\0') ? cursor : nullptr;

        char resolved_path[128];
        long pid = run_with_search(path_buf,
                                   g_current_cwd,
                                   args,
                                   0,
                                   false,
                                   resolved_path,
                                   sizeof(resolved_path));
        if (pid < 0) {
            print(console, "spawn: command not found: ");
            print_line(console, path_buf);
        } else {
            char message[48];
            char digits[20];
            uint64_to_string(static_cast<uint64_t>(pid), digits, sizeof(digits));
            const char prefix[] = "spawned pid ";
            size_t idx = 0;
            while (prefix[idx] != '\0' && idx + 1 < sizeof(message)) {
                message[idx] = prefix[idx];
                ++idx;
            }
            size_t digit_len = strlen(digits);
            for (size_t i = 0; i < digit_len && idx + 1 < sizeof(message); ++i) {
                message[idx++] = digits[i];
            }
            if (resolved_path[0] != '\0' && idx + 2 < sizeof(message)) {
                message[idx++] = ' ';
                message[idx++] = '(';
                size_t path_len_out = strlen(resolved_path);
                for (size_t i = 0; i < path_len_out && idx + 2 < sizeof(message); ++i) {
                    message[idx++] = resolved_path[i];
                }
                message[idx++] = ')';
            }
            message[idx] = '\0';
            print_line(console, message);
        }
    } else if (strcmp(command, "whoami") == 0) {
        print_line(console, g_session_user);
    } else if (strcmp(command, "help") == 0) {
        char path_info[192];
        size_t idx = 0;
        auto append_literal = [&](const char* text) {
            if (text == nullptr) {
                return;
            }
            for (size_t i = 0; text[i] != '\0' && idx + 1 < sizeof(path_info); ++i) {
                path_info[idx++] = text[i];
            }
        };
        append_literal("simple shell (PATH=.../binary:.../BINARY)");
        path_info[idx] = '\0';
        print_line(console, path_info);
        print_line(console, "builtins: cd, help, whoami, spawn, burst");
    } else if (strcmp(command, "burst") == 0) {
        // Spawn multiple no-op tasks without waiting.
        int spawned = 0;
        for (int i = 0; i < 5; ++i) {
            long r = run_with_search("noop",
                                     g_current_cwd,
                                     nullptr,
                                     0,
                                     false,
                                     nullptr,
                                     0);
            if (r >= 0) {
                ++spawned;
            }
        }
        print(console, "burst: spawned ");
        char buf[8];
        uint64_to_string(static_cast<uint64_t>(spawned), buf, sizeof(buf));
        print(console, buf);
        print_line(console, " noop task(s)");
    } else {
        const char* args = (cursor != nullptr && *cursor != '\0') ? cursor : nullptr;
        char resolved_path[128];
        long result = run_with_search(command,
                                      g_current_cwd,
                                      args,
                                      0,
                                      true,
                                      resolved_path,
                                      sizeof(resolved_path));
        if (result < 0) {
            print(console, "command not found: ");
            print_line(console, command);
        } else {
            const char* label = (resolved_path[0] != '\0') ? resolved_path : command;
            report_exec_result(console, result, label);
        }
    }
}

}  // namespace

int main(uint64_t arg, uint64_t) {
    const char* args = reinterpret_cast<const char*>(arg);
    parse_session_user(args);
    uint32_t vty_id = parse_vty_arg(args);
    if (vty_id != 0) {
        uint64_t flags =
            static_cast<uint64_t>(descriptor_defs::Flag::Readable) |
            static_cast<uint64_t>(descriptor_defs::Flag::Writable);
        uint64_t open_context =
            static_cast<uint64_t>(descriptor_defs::VtyOpen::Attach);
        (void)descriptor_open(kDescVty, vty_id, flags, open_context);
    }

    long std_input = process_get_standard_descriptor(0);
    long console = process_get_standard_descriptor(1);
    if (console < 0) {
        console = descriptor_open(kDescConsole, 0);
    }
    if (console < 0) return 1;

    long input_handle = std_input;
    if (input_handle < 0) {
        input_handle = descriptor_open(kDescKeyboard, 0);
        if (input_handle < 0) return 1;
    }

    long cwd_len = getcwd(g_current_cwd, sizeof(g_current_cwd));
    if (cwd_len < 0 || g_current_cwd[0] == '\0') {
        g_current_cwd[0] = '/';
        g_current_cwd[1] = '\0';
    }
    maybe_enter_home_directory();

    char prompt_buffer[160];
    size_t prompt_len = build_prompt(prompt_buffer, sizeof(prompt_buffer));
    descriptor_write(static_cast<uint32_t>(console), "\n", 1);
    write_prompt(console, prompt_buffer, prompt_len);

    char input_buffer[kMaxInputLength];
    size_t input_length = 0;
    input_buffer[0] = '\0';
    size_t rendered_length = prompt_len;

    bool input_is_keyboard = (std_input < 0);
    bool echo_input = std_input < 0;
    bool suppress_next_lf = false;
    uint8_t escape_state = 0;
    while (1) {
        auto handle_input_byte = [&](uint8_t key) {
            if (escape_state != 0) {
                if (escape_state == 1 && key == '[') {
                    escape_state = 2;
                    return;
                }
                if (escape_state == 2 && key >= 0x40 && key <= 0x7E) {
                    escape_state = 0;
                    return;
                }
                escape_state = 0;
                return;
            }
            if (key == 0x1B) {
                escape_state = 1;
                return;
            }
            if (key == '\n' && suppress_next_lf) {
                suppress_next_lf = false;
                return;
            }
            suppress_next_lf = false;
            if (key == '\r' || key == '\n') {
                suppress_next_lf = (key == '\r');
                if (echo_input) {
                    descriptor_write(static_cast<uint32_t>(console), "\n", 1);
                }
                input_buffer[input_length] = '\0';
                execute_command(console, input_buffer);
                input_length = 0;
                input_buffer[0] = '\0';
                prompt_len = build_prompt(prompt_buffer, sizeof(prompt_buffer));
                write_prompt(console, prompt_buffer, prompt_len);
                rendered_length = prompt_len;
            } else if (key == '\b' || key == 0x7F) {
                if (input_length > 0) {
                    --input_length;
                    input_buffer[input_length] = '\0';
                    prompt_len = build_prompt(prompt_buffer,
                                              sizeof(prompt_buffer));
                    if (echo_input) {
                        rendered_length = render_line(console,
                                                      prompt_buffer,
                                                      prompt_len,
                                                      input_buffer,
                                                      input_length,
                                                      rendered_length);
                    }
                }
            } else if (key == '\t') {
                // ignore tabs for now
            } else if (key >= 0x20 && key <= 0x7E) {
                if (input_length + 1 < sizeof(input_buffer)) {
                    input_buffer[input_length++] = static_cast<char>(key);
                    input_buffer[input_length] = '\0';
                    prompt_len = build_prompt(prompt_buffer,
                                              sizeof(prompt_buffer));
                    if (echo_input) {
                        rendered_length = render_line(console,
                                                      prompt_buffer,
                                                      prompt_len,
                                                      input_buffer,
                                                      input_length,
                                                      rendered_length);
                    }
                }
            }
        };

        if (input_is_keyboard) {
            descriptor_defs::KeyboardEvent events[8]{};
            long r = descriptor_read(static_cast<uint32_t>(input_handle),
                                     events,
                                     sizeof(events));
            if (r > 0) {
                size_t count =
                    static_cast<size_t>(r) / sizeof(events[0]);
                for (size_t i = 0; i < count; ++i) {
                    if (!keyboard::is_pressed(events[i])) {
                        continue;
                    }
                    if (keyboard::is_extended(events[i])) {
                        continue;
                    }
                    char ch = keyboard::scancode_to_char(events[i].scancode,
                                                         events[i].mods);
                    if (ch != 0) {
                        handle_input_byte(static_cast<uint8_t>(ch));
                    }
                }
            }
        } else {
            uint8_t key = 0;
            long r = descriptor_read(static_cast<uint32_t>(input_handle),
                                     &key,
                                     1);
            if (r > 0) {
                handle_input_byte(key);
            }
        }
        yield();
    }

    return 0;
}
