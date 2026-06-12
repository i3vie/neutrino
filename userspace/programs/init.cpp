#include <stddef.h>
#include <stdint.h>
#include <ctype.h>
#include <string.h>

#include "../crt/syscall.hpp"
#include "../auth/password_hash.hpp"
#include "keyboard_scancode.hpp"

namespace {

constexpr const char* kDefaultShellPath = ".../binary/shell.elf";
constexpr const char* kSpawnConfigPath = ".../config/spawn.cfg";
constexpr const char* kPrimaryUserStorePath = "/system/users.ntd";
constexpr const char* kFallbackUserStorePath = "/users.ntd";
constexpr size_t kConfigBufferSize = 1024;
constexpr size_t kMaxUserNameLength = 32;
constexpr size_t kMaxLoginUsers = 32;
constexpr const char* kRootUserName = "root";
constexpr uint64_t kAllCapabilities = ~0ull;

uint32_t g_console_handle = kInvalidDescriptor;

struct PrincipalCacheEntry {
    bool in_use;
    char name[kMaxUserNameLength];
    void* principal;
};

PrincipalCacheEntry g_principals[8]{};

struct UserStoreHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t entry_size;
    uint32_t count;
};

struct PackedUserV1 {
    char name[kMaxUserNameLength];
    uint64_t allowed_caps;
    uint64_t generation;
    uint8_t active;
    uint8_t reserved[7];
};

struct PackedUser {
    char name[kMaxUserNameLength];
    uint64_t allowed_caps;
    uint64_t generation;
    uint8_t active;
    uint8_t password_set;
    uint16_t password_algorithm;
    uint32_t password_iterations;
    uint8_t password_salt[auth::kPasswordSaltSize];
    uint8_t password_hash[auth::kPasswordHashSize];
    uint8_t reserved[24];
};

struct LoginUsers {
    char names[kMaxLoginUsers][kMaxUserNameLength];
    bool password_set[kMaxLoginUsers];
    uint32_t password_iterations[kMaxLoginUsers];
    uint8_t password_salt[kMaxLoginUsers][auth::kPasswordSaltSize];
    uint8_t password_hash[kMaxLoginUsers][auth::kPasswordHashSize];
    size_t count;
};

void print(const char* text) {
    if (g_console_handle == kInvalidDescriptor || text == nullptr) {
        return;
    }
    size_t length = strlen(text);
    if (length != 0) {
        descriptor_write(g_console_handle, text, length);
    }
}

void print_char(char ch) {
    if (g_console_handle == kInvalidDescriptor) {
        return;
    }
    descriptor_write(g_console_handle, &ch, 1);
}

char* skip_spaces(char* text) {
    if (text == nullptr) {
        return nullptr;
    }
    while (*text != '\0' && isspace(*text)) {
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
        if (!isspace(ch)) {
            break;
        }
        --end;
    }
    *end = '\0';
}

void zero_memory(void* ptr, size_t size) {
    auto* bytes = static_cast<uint8_t*>(ptr);
    for (size_t i = 0; i < size; ++i) {
        bytes[i] = 0;
    }
}

void* find_cached_principal(const char* user_name) {
    if (user_name == nullptr || user_name[0] == '\0') {
        return nullptr;
    }
    for (size_t i = 0; i < sizeof(g_principals) / sizeof(g_principals[0]); ++i) {
        if (!g_principals[i].in_use) {
            continue;
        }
        if (strcmp(g_principals[i].name, user_name) == 0) {
            return g_principals[i].principal;
        }
    }
    return nullptr;
}

bool cache_principal(const char* user_name, void* principal) {
    if (user_name == nullptr || user_name[0] == '\0' || principal == nullptr) {
        return false;
    }
    for (size_t i = 0; i < sizeof(g_principals) / sizeof(g_principals[0]); ++i) {
        if (!g_principals[i].in_use) {
            g_principals[i].in_use = true;
            strlcpy(g_principals[i].name,
                    user_name,
                    sizeof(g_principals[i].name));
            g_principals[i].principal = principal;
            return true;
        }
    }
    return false;
}

void* ensure_user_principal(const char* user_name) {
    if (user_name == nullptr || user_name[0] == '\0') {
        return nullptr;
    }

    void* principal = find_cached_principal(user_name);
    if (principal != nullptr) {
        return principal;
    }

    void* user = user_find(user_name);
    if (user == nullptr) {
        user = user_create(user_name, kAllCapabilities);
    }
    if (user == nullptr) {
        return nullptr;
    }

    principal = principal_create(user, kAllCapabilities);
    if (principal == nullptr) {
        return nullptr;
    }
    cache_principal(user_name, principal);
    return principal;
}

bool activate_user_principal(const char* user_name) {
    void* principal = ensure_user_principal(user_name);
    if (principal == nullptr) {
        print("init: failed to ensure principal for ");
        print(user_name);
        print("\n");
        return false;
    }
    if (principal_set(principal) < 0) {
        print("init: failed to set principal for ");
        print(user_name);
        print("\n");
        return false;
    }
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

bool set_console_color(uint32_t fg, uint32_t bg) {
    descriptor_defs::ColorPair colors{fg, bg};
    return descriptor_set_property(
               g_console_handle,
               static_cast<uint32_t>(descriptor_defs::Property::ConsoleColor),
               &colors,
               sizeof(colors)) == 0;
}

bool user_exists(const LoginUsers& users, const char* name) {
    if (name == nullptr || name[0] == '\0') {
        return false;
    }
    for (size_t i = 0; i < users.count; ++i) {
        if (strcmp(users.names[i], name) == 0) {
            return true;
        }
    }
    return false;
}

void ensure_login_user(LoginUsers& users, const char* name) {
    if (name == nullptr || name[0] == '\0' || user_exists(users, name)) {
        return;
    }
    if (users.count >= kMaxLoginUsers) {
        return;
    }
    strlcpy(users.names[users.count], name, sizeof(users.names[users.count]));
    users.password_set[users.count] = false;
    users.password_iterations[users.count] = 0;
    zero_memory(users.password_salt[users.count], auth::kPasswordSaltSize);
    zero_memory(users.password_hash[users.count], auth::kPasswordHashSize);
    ++users.count;
}

void set_login_user_password(LoginUsers& users,
                             const char* name,
                             uint32_t iterations,
                             const uint8_t* salt,
                             const uint8_t* hash) {
    if (name == nullptr || salt == nullptr || hash == nullptr || iterations == 0) {
        return;
    }
    for (size_t i = 0; i < users.count; ++i) {
        if (strcmp(users.names[i], name) != 0) {
            continue;
        }
        users.password_set[i] = true;
        users.password_iterations[i] = iterations;
        memcpy(users.password_salt[i], salt, auth::kPasswordSaltSize);
        memcpy(users.password_hash[i], hash, auth::kPasswordHashSize);
        return;
    }
}

bool load_login_users(LoginUsers& users) {
    users.count = 0;

    char buffer[4096];
    size_t len = 0;
    bool loaded = read_file_into_buffer(kPrimaryUserStorePath,
                                        buffer,
                                        sizeof(buffer),
                                        len);
    if (!loaded) {
        loaded = read_file_into_buffer(kFallbackUserStorePath,
                                       buffer,
                                       sizeof(buffer),
                                       len);
    }
    if (!loaded || len < sizeof(UserStoreHeader)) {
        ensure_login_user(users, kRootUserName);
        return users.count != 0;
    }

    UserStoreHeader header{};
    memcpy(&header, buffer, sizeof(header));
    if (header.magic != 0x4E544455u) {
        ensure_login_user(users, kRootUserName);
        return users.count != 0;
    }
    bool current = header.version == 2 &&
                   header.entry_size == sizeof(PackedUser);
    bool legacy = header.version == 1 &&
                  header.entry_size == sizeof(PackedUserV1);
    if (!current && !legacy) {
        ensure_login_user(users, kRootUserName);
        return users.count != 0;
    }

    size_t available =
        (len - sizeof(UserStoreHeader)) / header.entry_size;
    size_t count = header.count;
    if (count > available) {
        count = available;
    }
    if (count > kMaxLoginUsers) {
        count = kMaxLoginUsers;
    }

    if (legacy) {
        const PackedUserV1* entries =
            reinterpret_cast<const PackedUserV1*>(buffer + sizeof(UserStoreHeader));
        for (size_t i = 0; i < count; ++i) {
            if (entries[i].active == 0 || entries[i].name[0] == '\0') {
                continue;
            }
            ensure_login_user(users, entries[i].name);
        }
    } else {
        const PackedUser* entries =
            reinterpret_cast<const PackedUser*>(buffer + sizeof(UserStoreHeader));
        for (size_t i = 0; i < count; ++i) {
            if (entries[i].active == 0 || entries[i].name[0] == '\0') {
                continue;
            }
            ensure_login_user(users, entries[i].name);
            if (entries[i].password_set != 0 &&
                entries[i].password_algorithm == auth::kPasswordAlgorithmPbkdf2Sha256 &&
                entries[i].password_iterations != 0) {
                set_login_user_password(users,
                                        entries[i].name,
                                        entries[i].password_iterations,
                                        entries[i].password_salt,
                                        entries[i].password_hash);
            }
        }
    }

    ensure_login_user(users, kRootUserName);
    return users.count != 0;
}

bool read_login_name(char* out, size_t out_size) {
    if (out == nullptr || out_size == 0) {
        return false;
    }
    long keyboard = descriptor_open(
        static_cast<uint32_t>(descriptor_defs::Type::Keyboard), 0);
    if (keyboard < 0) {
        return false;
    }

    size_t length = 0;
    out[0] = '\0';
    for (;;) {
        descriptor_defs::KeyboardEvent events[8]{};
        long result = descriptor_read(static_cast<uint32_t>(keyboard),
                                      events,
                                      sizeof(events));
        if (result <= 0) {
            yield();
            continue;
        }
        size_t count = static_cast<size_t>(result) / sizeof(events[0]);
        for (size_t i = 0; i < count; ++i) {
            if (!keyboard::is_pressed(events[i]) ||
                keyboard::is_extended(events[i])) {
                continue;
            }
            char ch = keyboard::scancode_to_char(events[i].scancode,
                                                 events[i].mods);
            if (ch == '\r' || ch == '\n') {
                print("\n");
                descriptor_close(static_cast<uint32_t>(keyboard));
                out[length] = '\0';
                return length != 0;
            }
            if (ch == '\b' || ch == 0x7F) {
                if (length > 0) {
                    --length;
                    out[length] = '\0';
                    print("\b \b");
                }
                continue;
            }
            if (ch < 0x20 || ch > 0x7E) {
                continue;
            }
            if (length + 1 >= out_size) {
                continue;
            }
            out[length++] = ch;
            out[length] = '\0';
            print_char(ch);
        }
    }
}

bool read_secret_line(char* out, size_t out_size) {
    if (out == nullptr || out_size == 0) {
        return false;
    }
    long keyboard = descriptor_open(
        static_cast<uint32_t>(descriptor_defs::Type::Keyboard), 0);
    if (keyboard < 0) {
        return false;
    }

    size_t length = 0;
    out[0] = '\0';
    for (;;) {
        descriptor_defs::KeyboardEvent events[8]{};
        long result = descriptor_read(static_cast<uint32_t>(keyboard),
                                      events,
                                      sizeof(events));
        if (result <= 0) {
            yield();
            continue;
        }
        size_t count = static_cast<size_t>(result) / sizeof(events[0]);
        for (size_t i = 0; i < count; ++i) {
            if (!keyboard::is_pressed(events[i]) ||
                keyboard::is_extended(events[i])) {
                continue;
            }
            char ch = keyboard::scancode_to_char(events[i].scancode,
                                                 events[i].mods);
            if (ch == '\r' || ch == '\n') {
                print("\n");
                descriptor_close(static_cast<uint32_t>(keyboard));
                out[length] = '\0';
                return true;
            }
            if (ch == '\b' || ch == 0x7F) {
                if (length > 0) {
                    --length;
                    out[length] = '\0';
                }
                continue;
            }
            if (ch < 0x20 || ch > 0x7E) {
                continue;
            }
            if (length + 1 >= out_size) {
                continue;
            }
            out[length++] = ch;
            out[length] = '\0';
        }
    }
}

const LoginUsers* find_login_user(const LoginUsers& users,
                                  const char* name,
                                  size_t& out_index) {
    for (size_t i = 0; i < users.count; ++i) {
        if (strcmp(users.names[i], name) == 0) {
            out_index = i;
            return &users;
        }
    }
    return nullptr;
}

bool verify_login_password(const LoginUsers& users, size_t index) {
    if (index >= users.count || !users.password_set[index]) {
        return true;
    }
    print("password: ");
    char password[96];
    if (!read_secret_line(password, sizeof(password))) {
        return false;
    }
    uint8_t computed[auth::kPasswordHashSize];
    auth::pbkdf2_sha256(password,
                        users.password_salt[index],
                        auth::kPasswordSaltSize,
                        users.password_iterations[index],
                        computed);
    zero_memory(password, sizeof(password));
    bool ok = auth::constant_time_equal(computed,
                                        users.password_hash[index],
                                        auth::kPasswordHashSize);
    zero_memory(computed, sizeof(computed));
    return ok;
}

void build_shell_args(const char* user_name, char* out, size_t out_size) {
    if (out == nullptr || out_size == 0) {
        return;
    }
    strlcpy(out, "user=", out_size);
    size_t used = strlen(out);
    if (used + 1 >= out_size) {
        return;
    }
    strlcpy(out + used, user_name, out_size - used);
}

void run_login_loop() {
    set_console_color(0xFFF2F4F8u, 0x00000000u);
    for (;;) {
        LoginUsers users{};
        load_login_users(users);
        print("\n");
        print("login: ");

        char name[kMaxUserNameLength];
        if (!read_login_name(name, sizeof(name))) {
            print("Login cancelled\n");
            continue;
        }
        if (!user_exists(users, name)) {
            print("Unknown user\n");
            continue;
        }
        size_t user_index = 0;
        if (find_login_user(users, name, user_index) == nullptr ||
            !verify_login_password(users, user_index)) {
            print("Login incorrect\n");
            continue;
        }
        if (!activate_user_principal(name)) {
            print("Failed to activate user session\n");
            activate_user_principal(kRootUserName);
            continue;
        }

        char shell_args[48];
        build_shell_args(name, shell_args, sizeof(shell_args));
        long result = exec(kDefaultShellPath, shell_args, 0, nullptr);
        activate_user_principal(kRootUserName);
        if (result < 0) {
            print("Failed to start shell\n");
        }
    }
}

bool split_spawn_target(char* line, char*& command_out, const char*& user_out) {
    command_out = nullptr;
    user_out = kRootUserName;

    if (line == nullptr) {
        return false;
    }

    char* command = skip_spaces(line);
    if (command == nullptr || command[0] == '\0' || command[0] == '#') {
        return false;
    }

    if (command[0] == '@') {
        ++command;
        char* user_end = command;
        while (*user_end != '\0' && !isspace(*user_end)) {
            ++user_end;
        }
        if (user_end == command) {
            return false;
        }
        if (*user_end != '\0') {
            *user_end++ = '\0';
        }
        user_out = command;
        command = skip_spaces(user_end);
        if (command == nullptr || command[0] == '\0') {
            return false;
        }
    }

    command_out = command;
    return true;
}

bool spawn_command_line(char* line) {
    if (line == nullptr || line[0] == '\0') {
        return false;
    }

    char* command = nullptr;
    const char* user_name = nullptr;
    if (!split_spawn_target(line, command, user_name)) {
        return false;
    }

    if (!activate_user_principal(user_name)) {
        return false;
    }

    char* cursor = command;
    while (*cursor != '\0' && !isspace(*cursor)) {
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

    print("init: failed to spawn ");
    print(command);
    print("\n");
    return false;
}

bool spawn_from_config() {
    char buffer[kConfigBufferSize];
    size_t len = 0;
    if (!read_file_into_buffer(kSpawnConfigPath, buffer, sizeof(buffer), len)) {
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

    bool spawned_any = false;
    for (size_t i = 0; i < line_count; ++i) {
        if (spawn_command_line(lines[i])) {
            spawned_any = true;
        }
    }
    return spawned_any;
}

}  // namespace

int main(uint64_t, uint64_t) {
    long console = descriptor_open(
        static_cast<uint32_t>(descriptor_defs::Type::Console), 0);
    if (console >= 0) {
        g_console_handle = static_cast<uint32_t>(console);
    }

    activate_user_principal(kRootUserName);
    (void)ensure_user_principal(kRootUserName);
    (void)spawn_from_config();
    run_login_loop();
}
