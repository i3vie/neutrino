#include <stddef.h>
#include <stdint.h>

#include "../crt/syscall.hpp"
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
        print("usage: userctl create <name> <capmask>|find <name>|bump <name>\n");
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

    print("unknown command\n");
    return 1;
}
