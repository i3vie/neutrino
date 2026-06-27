#include "text.hpp"

#include <string.h>

namespace userspace::text {

bool starts_with(const char* text, const char* prefix) {
    return text != nullptr &&
           prefix != nullptr &&
           strncmp(text, prefix, strlen(prefix)) == 0;
}

bool append_char(char* out, size_t out_size, size_t& len, char ch) {
    if (out == nullptr || len + 1 >= out_size) {
        return false;
    }
    out[len++] = ch;
    out[len] = '\0';
    return true;
}

bool append_text(char* out, size_t out_size, size_t& len, const char* text) {
    if (text == nullptr) {
        return false;
    }
    while (*text != '\0') {
        if (!append_char(out, out_size, len, *text++)) {
            return false;
        }
    }
    return true;
}

}  // namespace userspace::text
