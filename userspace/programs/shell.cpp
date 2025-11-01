// build with:
// g++ -ffreestanding -fno-builtin -fno-stack-protector -nostdlib -m64 -fPIE -pie -I../crt ../build/crt0.o shell.cpp -o shell.elf

#include "../crt/syscall.hpp"
#include <stddef.h>
#include <stdint.h>

#define DESC_TYPE_CONSOLE  1  // In the future, these would ideally be in
#define DESC_TYPE_KEYBOARD 3  // kernel-provided headers

namespace {

constexpr size_t kMaxInputLength = 256;
constexpr size_t kMaxCommandLength = 128;
constexpr size_t kPathMax = 128;
constexpr size_t kMaxSegments = 64;
constexpr size_t kMaxSearchDirs = 8;
struct PathSegment {
    const char* data;
    size_t length;
};

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

void copy_string(char* dest, size_t dest_size, const char* src) {
    if (dest == nullptr || dest_size == 0) {
        return;
    }
    size_t idx = 0;
    if (src != nullptr) {
        while (src[idx] != '\0' && idx + 1 < dest_size) {
            dest[idx] = src[idx];
            ++idx;
        }
    }
    dest[idx] = '\0';
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

char g_current_cwd[128] = "/";

bool is_space(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

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

bool equals_literal(const char* value, const char* literal) {
    if (value == nullptr || literal == nullptr) {
        return false;
    }
    size_t idx = 0;
    while (literal[idx] != '\0') {
        if (value[idx] != literal[idx]) {
            return false;
        }
        ++idx;
    }
    return value[idx] == '\0';
}

void extract_mount_name(const char* path, char* out, size_t out_size) {
    if (out == nullptr || out_size == 0) {
        return;
    }
    out[0] = '\0';
    if (path == nullptr || path[0] != '/') {
        return;
    }
    size_t src = 1;
    size_t dst = 0;
    while (path[src] != '\0' && path[src] != '/') {
        if (dst + 1 >= out_size) {
            out[0] = '\0';
            return;
        }
        out[dst++] = path[src++];
    }
    if (dst == 0) {
        out[0] = '\0';
        return;
    }
    out[dst] = '\0';
}

bool build_mount_subpath(const char* mount,
                         const char* suffix,
                         char* out,
                         size_t out_size) {
    if (out == nullptr || out_size == 0) {
        return false;
    }
    if (mount == nullptr || mount[0] == '\0') {
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

size_t build_search_directories(const char* cwd,
                                char (&out)[kMaxSearchDirs][128]) {
    constexpr const char* kFallbackMount = "IDE_PM_0";
    size_t count = 0;

    auto append = [&](const char* path) {
        if (path == nullptr || path[0] == '\0') {
            return;
        }
        if (count >= kMaxSearchDirs) {
            return;
        }
        copy_string(out[count], sizeof(out[count]), path);
        ++count;
    };

    auto append_mount_dirs = [&](const char* mount) {
        if (mount == nullptr || mount[0] == '\0') {
            return;
        }
        char buffer[128];
        if (build_mount_subpath(mount, "binary", buffer, sizeof(buffer))) {
            append(buffer);
        }
        if (build_mount_subpath(mount, "BINARY", buffer, sizeof(buffer))) {
            append(buffer);
        }
    };

    char mount_name[64];
    extract_mount_name(cwd, mount_name, sizeof(mount_name));
    if (mount_name[0] != '\0') {
        append_mount_dirs(mount_name);
    }
    if (!strings_equal(mount_name, kFallbackMount)) {
        append_mount_dirs(kFallbackMount);
    }
    append("/binary");
    append("/BINARY");

    return count;
}

bool parse_segments(const char* path,
                    bool path_is_absolute,
                    PathSegment (&segments)[kMaxSegments],
                    size_t& count) {
    if (path == nullptr) {
        return true;
    }

    const char* cursor = path;
    while (*cursor != '\0') {
        while (*cursor == '/') {
            ++cursor;
        }
        if (*cursor == '\0') {
            break;
        }

        const char* start = cursor;
        while (*cursor != '\0' && *cursor != '/') {
            ++cursor;
        }
        size_t len = static_cast<size_t>(cursor - start);
        if (len == 0) {
            continue;
        }
        if (len == 1 && start[0] == '.') {
            continue;
        }
        if (len == 2 && start[0] == '.' && start[1] == '.') {
            if (count > 0) {
                --count;
            } else if (!path_is_absolute) {
                // ignore attempts to traverse above the root of the
                // combined path for relative inputs
            }
            continue;
        }
        if (count >= kMaxSegments) {
            return false;
        }
        segments[count++] = PathSegment{start, len};
    }
    return true;
}

bool write_segments(const PathSegment (&segments)[kMaxSegments],
                    size_t count,
                    char* out,
                    size_t out_size) {
    if (out == nullptr || out_size == 0) {
        return false;
    }
    if (out_size < 2) {
        return false;
    }

    size_t length = 0;
    out[length++] = '/';
    for (size_t i = 0; i < count; ++i) {
        if (length > 1) {
            if (length + 1 >= out_size) {
                return false;
            }
            out[length++] = '/';
        }
        if (length + segments[i].length >= out_size) {
            return false;
        }
        for (size_t j = 0; j < segments[i].length; ++j) {
            out[length++] = segments[i].data[j];
        }
    }
    if (length > 1 && out[length - 1] == '/') {
        --length;
    }
    if (length >= out_size) {
        return false;
    }
    out[length] = '\0';
    return true;
}

bool build_absolute_path_user(const char* base,
                              const char* input,
                              char* out,
                              size_t out_size) {
    PathSegment segments[kMaxSegments];
    size_t segment_count = 0;

    const char* effective_base =
        (base != nullptr && base[0] != '\0') ? base : "/";
    if (!parse_segments(effective_base, true, segments, segment_count)) {
        return false;
    }

    if (input == nullptr || input[0] == '\0') {
        return write_segments(segments, segment_count, out, out_size);
    }

    if (input[0] == '/') {
        segment_count = 0;
        if (!parse_segments(input, true, segments, segment_count)) {
            return false;
        }
        return write_segments(segments, segment_count, out, out_size);
    }

    if (!parse_segments(input, false, segments, segment_count)) {
        return false;
    }
    return write_segments(segments, segment_count, out, out_size);
}

bool resolve_path(const char* base,
                  const char* input,
                  char* out,
                  size_t out_size) {
    if (out == nullptr || out_size == 0) {
        return false;
    }

    if (input == nullptr) {
        return build_absolute_path_user(base, nullptr, out, out_size);
    }

    if (string_length(input) >= kPathMax) {
        return false;
    }

    char input_copy[kPathMax];
    copy_string(input_copy, sizeof(input_copy), input);
    return build_absolute_path_user(base, input_copy, out, out_size);
}

size_t build_prompt(char* buffer, size_t buffer_size) {
    if (buffer == nullptr || buffer_size < 3) {
        return 0;
    }
    size_t cwd_len = string_length(g_current_cwd);
    if (cwd_len + 3 > buffer_size) {
        if (buffer_size <= 3) {
            buffer[0] = '>';
            buffer[1] = ' ';
            buffer[2] = '\0';
            return 2;
        }
        cwd_len = buffer_size - 3;
    }
    size_t idx = 0;
    for (size_t i = 0; i < cwd_len; ++i) {
        buffer[idx++] = g_current_cwd[i];
    }
    buffer[idx++] = ' ';
    buffer[idx++] = '>';
    buffer[idx++] = ' ';
    buffer[idx] = '\0';
    return idx;
}

bool join_path(char* dest,
               size_t dest_size,
               const char* dir,
               const char* cmd,
               const char* suffix) {
    if (dest == nullptr || dest_size == 0 || dir == nullptr || cmd == nullptr) {
        return false;
    }
    size_t dir_len = string_length(dir);
    size_t cmd_len = string_length(cmd);
    size_t suffix_len = (suffix != nullptr) ? string_length(suffix) : 0;
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
        char resolved[128];
        if (!resolve_path(cwd, command, resolved, sizeof(resolved))) {
            return -1;
        }
        long value = invoke_exec(resolved, args, flags);
        if (value >= 0 && resolved_path != nullptr) {
            copy_string(resolved_path, resolved_path_size, resolved);
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
                    copy_string(resolved_path, resolved_path_size, candidate);
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
                    copy_string(resolved_path, resolved_path_size, candidate);
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
    size_t label_len = string_length(label);
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
    size_t digit_len = string_length(digits);
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
    while (is_space(*str)) {
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

    char command[kMaxCommandLength];
   size_t cmd_len = 0;
    while (*cursor != '\0' && !is_space(*cursor)) {
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

    if (equals_literal(command, "cd")) {
        const char* target_start = skip_spaces(cursor);
        char target_buf[128];
        size_t target_len = 0;
        if (target_start != nullptr && *target_start != '\0') {
            while (target_start[target_len] != '\0' &&
                   !is_space(target_start[target_len])) {
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

        char resolved[128];
        if (!resolve_path(g_current_cwd, target_buf, resolved, sizeof(resolved))) {
            print_line(console, "cd: path too long");
            return;
        }
        long dir = directory_open(resolved);
        if (dir < 0) {
            print(console, "cd: no such directory: ");
            print_line(console, resolved);
            return;
        }
        directory_close(static_cast<uint32_t>(dir));
        copy_string(g_current_cwd, sizeof(g_current_cwd), resolved);
        if (setcwd(g_current_cwd) < 0) {
            print_line(console, "cd: failed to update process cwd");
        }
        return;
    } else if (equals_literal(command, "spawn")) {
        cursor = skip_spaces(cursor);
        if (cursor == nullptr || *cursor == '\0') {
            print_line(console, "usage: spawn <path> [args]");
            return;
        }

        const char* path_start = cursor;
        size_t path_len = 0;
        while (cursor[path_len] != '\0' && !is_space(cursor[path_len])) {
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
            size_t digit_len = string_length(digits);
            for (size_t i = 0; i < digit_len && idx + 1 < sizeof(message); ++i) {
                message[idx++] = digits[i];
            }
            if (resolved_path[0] != '\0' && idx + 2 < sizeof(message)) {
                message[idx++] = ' ';
                message[idx++] = '(';
                size_t path_len_out = string_length(resolved_path);
                for (size_t i = 0; i < path_len_out && idx + 2 < sizeof(message); ++i) {
                    message[idx++] = resolved_path[i];
                }
                message[idx++] = ')';
            }
            message[idx] = '\0';
            print_line(console, message);
        }
    } else if (equals_literal(command, "help")) {
        print_line(console, "simple shell (PATH=/binary:IDE_PM_0/BINARY)");
        print_line(console, "builtins: cd, help, spawn");
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

int main(uint64_t, uint64_t) {
    long console = descriptor_open(DESC_TYPE_CONSOLE, 0);
    if (console < 0) return 1;

    long keyboard = descriptor_open(DESC_TYPE_KEYBOARD, 0);
    if (keyboard < 0) return 1;

    char prompt_buffer[160];
    size_t prompt_len = build_prompt(prompt_buffer, sizeof(prompt_buffer));
    descriptor_write(static_cast<uint32_t>(console), "\n", 1);
    descriptor_write(static_cast<uint32_t>(console), prompt_buffer, prompt_len);

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
            prompt_len = build_prompt(prompt_buffer, sizeof(prompt_buffer));
            descriptor_write(static_cast<uint32_t>(console),
                             prompt_buffer,
                             prompt_len);
            rendered_length = prompt_len;
            } else if (key == '\b' || key == 0x7F) {
                if (input_length > 0) {
                    --input_length;
                    input_buffer[input_length] = '\0';
                    prompt_len = build_prompt(prompt_buffer, sizeof(prompt_buffer));
                    rendered_length = render_line(console,
                                                  prompt_buffer,
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
                    prompt_len = build_prompt(prompt_buffer, sizeof(prompt_buffer));
                    rendered_length = render_line(console,
                                                  prompt_buffer,
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
