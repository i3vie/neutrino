#include "args.hpp"

namespace userspace {

bool is_space(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
}

const char* skip_spaces(const char* text) {
    while (text != nullptr && is_space(*text)) {
        ++text;
    }
    return text;
}

bool copy_token(const char*& cursor, char* out, size_t out_size) {
    if (out == nullptr || out_size == 0) {
        return false;
    }
    cursor = skip_spaces(cursor);
    if (cursor == nullptr || *cursor == '\0') {
        return false;
    }

    size_t len = 0;
    while (cursor[len] != '\0' && !is_space(cursor[len])) {
        if (len + 1 >= out_size) {
            return false;
        }
        out[len] = cursor[len];
        ++len;
    }
    out[len] = '\0';
    cursor += len;
    return true;
}

bool only_spaces_remain(const char* text) {
    text = skip_spaces(text);
    return text == nullptr || *text == '\0';
}

}  // namespace userspace
