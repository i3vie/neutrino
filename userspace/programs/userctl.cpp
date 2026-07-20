#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../crt/syscall.hpp"
#include "../auth/password_hash.hpp"
#include "keyboard_scancode.hpp"
#include "descriptors.hpp"

namespace {

void print(const char* s) {
    static int32_t console = -1;
    if (console < 0) {
        console = static_cast<int32_t>(
            descriptor_open(static_cast<uint32_t>(descriptor_defs::Type::Console), 0));
        if (console < 0) return;
    }
    if (s == nullptr) return;
    size_t len = 0;
    while (s[len] != '\0') ++len;
    if (len == 0) return;
    descriptor_write(static_cast<uint32_t>(console), s, len);
}

void u64_to_hex(uint64_t v, char* out, size_t out_sz) {
    const char* digits = "0123456789ABCDEF";
    if (out_sz < 3) return;
    size_t idx = 0;
    out[idx++] = '0';
    out[idx++] = 'x';
    for (int i = 15; i >= 0 && idx < out_sz; --i) {
        out[idx++] = digits[(v >> (i * 4)) & 0xF];
    }
    if (idx < out_sz) out[idx] = '\0';
    else out[out_sz - 1] = '\0';
}

void print_hex(const char* label, uint64_t v) {
    char buf[20];
    u64_to_hex(v, buf, sizeof(buf));
    print(label); print(buf); print("\n");
}

void u64_to_dec(uint64_t v, char* out, size_t out_sz) {
    if (out_sz == 0) return;
    char temp[32];
    size_t idx = 0;
    if (v == 0) {
        temp[idx++] = '0';
    } else {
        while (v > 0 && idx < sizeof(temp)) {
            temp[idx++] = static_cast<char>('0' + (v % 10));
            v /= 10;
        }
    }
    size_t n = idx < out_sz - 1 ? idx : out_sz - 1;
    for (size_t i = 0; i < n; ++i) {
        out[i] = temp[idx - 1 - i];
    }
    out[n] = '\0';
}

void print_dec(const char* label, uint64_t v) {
    char buf[32];
    u64_to_dec(v, buf, sizeof(buf));
    print(label); print(buf); print("\n");
}

bool parse_u64(const char* s, uint64_t& out) {
    if (!s || !*s) return false;
    uint64_t val = 0;
    while (*s) {
        char c = *s++;
        if (c < '0' || c > '9') return false;
        val = val * 10 + static_cast<uint64_t>(c - '0');
    }
    out = val;
    return true;
}

void zero_memory(void* ptr, size_t size) {
    auto* bytes = static_cast<uint8_t*>(ptr);
    for (size_t i = 0; i < size; ++i) {
        bytes[i] = 0;
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

bool read_new_password(char* password, size_t password_size) {
    char confirm[96];
    print("new password: ");
    if (!read_secret_line(password, password_size) || password[0] == '\0') {
        print("empty password not allowed\n");
        return false;
    }
    print("confirm password: ");
    if (!read_secret_line(confirm, sizeof(confirm))) {
        zero_memory(password, password_size);
        return false;
    }
    bool ok = strcmp(password, confirm) == 0;
    zero_memory(confirm, sizeof(confirm));
    if (!ok) {
        zero_memory(password, password_size);
        print("passwords do not match\n");
    }
    return ok;
}

}  // namespace

extern "C" int main(uint64_t arg_ptr, uint64_t /*flags*/) {
    const char* args = reinterpret_cast<const char*>(arg_ptr);

    // Tokenize simple space-separated string (up to 4 tokens)
    const char* tok_start[4] = {nullptr, nullptr, nullptr, nullptr};
    size_t tok_len[4] = {0, 0, 0, 0};
    size_t tok_count = 0;

    const char* cur = args;
    while (cur && *cur && tok_count < 4) {
        while (*cur == ' ') ++cur;
        if (*cur == '\0') break;
        tok_start[tok_count] = cur;
        while (*cur && *cur != ' ') {
            ++tok_len[tok_count];
            ++cur;
        }
        ++tok_count;
    }

    auto token_equals = [&](size_t idx, const char* lit) -> bool {
        size_t i = 0;
        while (lit[i] && i < tok_len[idx] && tok_start[idx][i] == lit[i]) ++i;
        return lit[i] == '\0' && i == tok_len[idx];
    };

    auto copy_token = [&](size_t idx, char* out, size_t out_sz) {
        if (out_sz == 0) return;
        size_t n = tok_len[idx] < out_sz - 1 ? tok_len[idx] : out_sz - 1;
        for (size_t i = 0; i < n; ++i) out[i] = tok_start[idx][i];
        out[n] = '\0';
    };

    auto parse_token_u64 = [&](size_t idx, uint64_t& out) -> bool {
        char buf[32];
        copy_token(idx, buf, sizeof(buf));
        return parse_u64(buf, out);
    };

    if (tok_count < 1) {
        print("usage: userctl create <name> <capmask>|find <name>|bump <name>|passwd <name>|info <name>\n");
        return 1;
    }

    if (token_equals(0, "create")) {
        if (tok_count < 3) {
            print("create needs <name> <capmask>\n");
            return 1;
        }
        char name_buf[33];
        copy_token(1, name_buf, sizeof(name_buf));
        uint64_t mask = 0;
        if (!parse_token_u64(2, mask)) {
            print("invalid capmask\n");
            return 1;
        }
        void* user = user_create(name_buf, mask);
        if (!user) {
            print("create failed (maybe exists or full)\n");
            return 1;
        }
        print_hex("user=", reinterpret_cast<uint64_t>(user));
        return 0;
    }

    if (token_equals(0, "find")) {
        if (tok_count < 2) {
            print("find needs <name>\n");
            return 1;
        }
        char name_buf[33];
        copy_token(1, name_buf, sizeof(name_buf));
        void* user = user_find(name_buf);
        if (!user) {
            print("not found\n");
            return 1;
        }
        print_hex("user=", reinterpret_cast<uint64_t>(user));
        return 0;
    }

    if (token_equals(0, "bump")) {
        if (tok_count < 2) {
            print("bump needs <name>\n");
            return 1;
        }
        char name_buf[33];
        copy_token(1, name_buf, sizeof(name_buf));
        void* user = user_find(name_buf);
        if (!user) {
            print("not found\n");
            return 1;
        }
        long r = user_bump_generation(user);
        if (r < 0) {
            print("bump failed\n");
            return 1;
        }
        print("bumped\n");
        return 0;
    }

    if (token_equals(0, "passwd")) {
        if (tok_count < 2) {
            print("passwd needs <name>\n");
            return 1;
        }
        char name_buf[33];
        copy_token(1, name_buf, sizeof(name_buf));
        void* user = user_find(name_buf);
        if (!user) {
            print("not found\n");
            return 1;
        }
        char password[96];
        if (!read_new_password(password, sizeof(password))) {
            return 1;
        }
        uint8_t salt[auth::kPasswordSaltSize];
        uint8_t hash[auth::kPasswordHashSize];
        if (!auth::make_salt(name_buf, salt)) {
            print("secure random source unavailable\n");
            zero_memory(password, sizeof(password));
            return 1;
        }
        auth::pbkdf2_sha256(password,
                            salt,
                            auth::kPasswordSaltSize,
                            auth::kPasswordIterations,
                            hash);
        zero_memory(password, sizeof(password));
        long r = user_set_password(user, salt, hash, auth::kPasswordIterations);
        zero_memory(hash, sizeof(hash));
        if (r < 0) {
            print("passwd failed\n");
            return 1;
        }
        print("password updated\n");
        return 0;
    }

    if (token_equals(0, "info")) {
        if (tok_count < 2) {
            print("info needs <name>\n");
            return 1;
        }
        char name_buf[33];
        copy_token(1, name_buf, sizeof(name_buf));
        void* user = user_find(name_buf);
        if (!user) {
            print("not found\n");
            return 1;
        }
        UserInfo info;
        long r = user_info(user, &info);
        if (r < 0) {
            print("info failed\n");
            return 1;
        }
        print("name: "); print(info.name); print("\n");
        print_dec("id.machine: ", info.id_machine);
        print_dec("id.local: ", info.id_local);
        print_hex("allowed_caps: ", info.allowed_caps);
        print_dec("generation: ", info.generation);
        print_dec("password_set: ", info.password_set);
        print_dec("active: ", info.active);
        return 0;
    }

    print("unknown command\n");
    return 1;
}
