#include "http.hpp"

#include <string.h>

namespace userspace::http {

bool is_digit(char ch) {
    return ch >= '0' && ch <= '9';
}

char to_lower(char ch) {
    if (ch >= 'A' && ch <= 'Z') {
        return static_cast<char>(ch - 'A' + 'a');
    }
    return ch;
}

bool starts_with_ci(const char* text, const char* prefix) {
    if (text == nullptr || prefix == nullptr) {
        return false;
    }
    while (*prefix != '\0') {
        if (*text == '\0' || to_lower(*text) != to_lower(*prefix)) {
            return false;
        }
        ++text;
        ++prefix;
    }
    return true;
}

int find_char(const char* text, char ch) {
    if (text == nullptr) {
        return -1;
    }
    for (size_t i = 0; text[i] != '\0'; ++i) {
        if (text[i] == ch) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void copy_range(char* dest, size_t capacity, const char* begin, const char* end) {
    if (dest == nullptr || capacity == 0) {
        return;
    }
    size_t length = 0;
    if (begin != nullptr && end != nullptr && begin <= end) {
        length = static_cast<size_t>(end - begin);
        if (length >= capacity) {
            length = capacity - 1;
        }
        if (length != 0) {
            memcpy(dest, begin, length);
        }
    }
    dest[length] = '\0';
}

bool append_cstr(char* dest, size_t capacity, const char* src) {
    if (dest == nullptr || capacity == 0 || src == nullptr) {
        return false;
    }
    size_t length = strlen(dest);
    if (length >= capacity) {
        return false;
    }
    size_t i = 0;
    while (src[i] != '\0' && length + 1 < capacity) {
        dest[length++] = src[i++];
    }
    dest[length] = '\0';
    return src[i] == '\0';
}

void trim_spaces(char* text) {
    if (text == nullptr) {
        return;
    }
    size_t start = 0;
    while (text[start] == ' ' || text[start] == '\t') {
        ++start;
    }
    size_t length = strlen(text + start);
    memmove(text, text + start, length + 1);
    while (length != 0 &&
           (text[length - 1] == ' ' || text[length - 1] == '\t')) {
        text[--length] = '\0';
    }
}

bool parse_u16_range(const char* begin, const char* end, uint16_t& out) {
    if (begin == nullptr || end == nullptr || begin == end) {
        return false;
    }
    uint32_t value = 0;
    for (const char* cursor = begin; cursor != end; ++cursor) {
        if (!is_digit(*cursor)) {
            return false;
        }
        value = value * 10u + static_cast<uint32_t>(*cursor - '0');
        if (value > 65535u) {
            return false;
        }
    }
    out = static_cast<uint16_t>(value);
    return true;
}

bool parse_decimal(const char* text, size_t& out) {
    if (text == nullptr || *text == '\0') {
        return false;
    }
    size_t value = 0;
    while (*text != '\0') {
        if (!is_digit(*text)) {
            return false;
        }
        value = value * 10u + static_cast<size_t>(*text - '0');
        ++text;
    }
    out = value;
    return true;
}

int parse_decimal_int(const char* text) {
    size_t value = 0;
    if (!parse_decimal(text, value) || value > static_cast<size_t>(0x7FFFFFFF)) {
        return -1;
    }
    return static_cast<int>(value);
}

bool parse_decimal_range(const char* begin, const char* end, int& out) {
    if (begin == nullptr || end == nullptr || begin >= end) {
        return false;
    }
    int value = 0;
    for (const char* cursor = begin; cursor != end; ++cursor) {
        if (!is_digit(*cursor)) {
            return false;
        }
        value = value * 10 + static_cast<int>(*cursor - '0');
    }
    out = value;
    return true;
}

bool parse_hex_size(const char* text, size_t& out) {
    if (text == nullptr || *text == '\0') {
        return false;
    }
    size_t value = 0;
    while (*text != '\0' && *text != ';') {
        char ch = to_lower(*text);
        uint8_t digit = 0;
        if (ch >= '0' && ch <= '9') {
            digit = static_cast<uint8_t>(ch - '0');
        } else if (ch >= 'a' && ch <= 'f') {
            digit = static_cast<uint8_t>(10 + ch - 'a');
        } else {
            return false;
        }
        value = (value << 4) | digit;
        ++text;
    }
    out = value;
    return true;
}

bool parse_url(const char* text, Url& out, UrlParseMode mode) {
    if (text == nullptr || *text == '\0') {
        return false;
    }

    const char* cursor = text;
    if (starts_with_ci(cursor, "http://")) {
        out.scheme = Scheme::Http;
        out.port = 80;
        cursor += 7;
    } else if (starts_with_ci(cursor, "https://")) {
        out.scheme = Scheme::Https;
        out.port = 443;
        cursor += 8;
    } else if (mode == UrlParseMode::DefaultHttps) {
        out.scheme = Scheme::Https;
        out.port = 443;
    } else if (mode == UrlParseMode::DefaultHttp) {
        out.scheme = Scheme::Http;
        out.port = 80;
    } else {
        return false;
    }

    const char* host_begin = cursor;
    while (*cursor != '\0' && *cursor != '/' && *cursor != ':') {
        ++cursor;
    }
    if (cursor == host_begin) {
        return false;
    }
    copy_range(out.host, sizeof(out.host), host_begin, cursor);

    if (*cursor == ':') {
        ++cursor;
        const char* port_begin = cursor;
        while (*cursor != '\0' && *cursor != '/') {
            ++cursor;
        }
        if (!parse_u16_range(port_begin, cursor, out.port) || out.port == 0) {
            return false;
        }
    }

    if (*cursor == '\0') {
        strlcpy(out.path, "/", sizeof(out.path));
    } else {
        strlcpy(out.path, cursor, sizeof(out.path));
    }
    return true;
}

bool parse_ipv4_component(const char* begin, const char* end, uint8_t& out) {
    if (begin == end) {
        return false;
    }
    uint32_t value = 0;
    for (const char* cursor = begin; cursor != end; ++cursor) {
        if (!is_digit(*cursor)) {
            return false;
        }
        value = value * 10u + static_cast<uint32_t>(*cursor - '0');
        if (value > 255u) {
            return false;
        }
    }
    out = static_cast<uint8_t>(value);
    return true;
}

bool parse_ipv4_literal(const char* text, uint8_t out[4]) {
    if (text == nullptr || *text == '\0' || out == nullptr) {
        return false;
    }
    const char* component = text;
    for (size_t i = 0; i < 4; ++i) {
        const char* cursor = component;
        while (*cursor != '\0' && *cursor != '.') {
            ++cursor;
        }
        if (!parse_ipv4_component(component, cursor, out[i])) {
            return false;
        }
        if (i != 3) {
            if (*cursor != '.') {
                return false;
            }
            component = cursor + 1;
        } else if (*cursor != '\0') {
            return false;
        }
    }
    return true;
}

void append_port(char* out, size_t out_size, uint16_t port) {
    size_t length = strlen(out);
    if (length + 1 >= out_size) {
        return;
    }
    out[length++] = ':';
    char digits[8];
    size_t count = 0;
    do {
        digits[count++] = static_cast<char>('0' + (port % 10u));
        port = static_cast<uint16_t>(port / 10u);
    } while (port != 0 && count < sizeof(digits));
    while (count != 0 && length + 1 < out_size) {
        out[length++] = digits[--count];
    }
    out[length] = '\0';
}

bool url_to_string(const Url& url, char* out, size_t out_size) {
    if (out == nullptr || out_size == 0) {
        return false;
    }
    const char* scheme = url.scheme == Scheme::Https ? "https://" : "http://";
    if (strlen(scheme) + strlen(url.host) + strlen(url.path) + 8 >= out_size) {
        return false;
    }
    strlcpy(out, scheme, out_size);
    if (!append_cstr(out, out_size, url.host)) {
        return false;
    }
    bool need_port = (url.scheme == Scheme::Http && url.port != 80) ||
                     (url.scheme == Scheme::Https && url.port != 443);
    if (need_port) {
        append_port(out, out_size, url.port);
    }
    return append_cstr(out, out_size, url.path);
}

bool build_redirect_url(const Url& current,
                        const char* location,
                        char* out,
                        size_t out_size) {
    if (location == nullptr || out == nullptr || out_size == 0 || *location == '\0') {
        return false;
    }
    if (starts_with_ci(location, "http://") || starts_with_ci(location, "https://")) {
        strlcpy(out, location, out_size);
        return strlen(location) < out_size;
    }
    if (location[0] == '/' && location[1] == '/') {
        const char* scheme = current.scheme == Scheme::Https ? "https:" : "http:";
        if (strlen(scheme) + strlen(location) >= out_size) {
            return false;
        }
        strlcpy(out, scheme, out_size);
        return append_cstr(out, out_size, location);
    }
    if (location[0] == '/') {
        Url root = current;
        strlcpy(root.path, location, sizeof(root.path));
        return url_to_string(root, out, out_size);
    }

    Url relative = current;
    const char* slash = relative.path + strlen(relative.path);
    while (slash > relative.path && slash[-1] != '/') {
        --slash;
    }
    char base[kMaxPath];
    copy_range(base, sizeof(base), relative.path, slash);
    if (strlen(base) + strlen(location) >= sizeof(relative.path)) {
        return false;
    }
    strlcpy(relative.path, base, sizeof(relative.path));
    if (!append_cstr(relative.path, sizeof(relative.path), location)) {
        return false;
    }
    return url_to_string(relative, out, out_size);
}

void init_response_meta(ResponseMeta& meta) {
    meta.status_code = 0;
    meta.chunked = false;
    meta.have_content_length = false;
    meta.content_length = 0;
    meta.is_html = false;
    meta.is_text = false;
    meta.location[0] = '\0';
}

bool read_response_headers(void* context, ReadLineFn read_line, ResponseMeta& meta) {
    if (read_line == nullptr) {
        return false;
    }
    init_response_meta(meta);
    char line[1024];
    if (!read_line(context, line, sizeof(line))) {
        return false;
    }
    int first_space = find_char(line, ' ');
    if (first_space >= 0) {
        const char* status_begin = line + first_space + 1;
        const char* status_end = status_begin;
        while (*status_end != '\0' && *status_end != ' ') {
            ++status_end;
        }
        int status_code = 0;
        if (parse_decimal_range(status_begin, status_end, status_code)) {
            meta.status_code = status_code;
        }
    }
    for (;;) {
        if (!read_line(context, line, sizeof(line))) {
            return false;
        }
        if (line[0] == '\0') {
            return true;
        }
        int colon = find_char(line, ':');
        if (colon <= 0) {
            continue;
        }
        line[colon] = '\0';
        char* value = line + colon + 1;
        trim_spaces(value);
        if (starts_with_ci(line, "content-type")) {
            if (starts_with_ci(value, "text/html") ||
                starts_with_ci(value, "application/xhtml+xml")) {
                meta.is_html = true;
                meta.is_text = true;
            } else if (starts_with_ci(value, "text/") ||
                       starts_with_ci(value, "application/json") ||
                       starts_with_ci(value, "application/xml")) {
                meta.is_text = true;
            }
        } else if (starts_with_ci(line, "content-length")) {
            size_t parsed = 0;
            if (parse_decimal(value, parsed)) {
                meta.have_content_length = true;
                meta.content_length = parsed;
            }
        } else if (starts_with_ci(line, "transfer-encoding")) {
            if (starts_with_ci(value, "chunked")) {
                meta.chunked = true;
            }
        } else if (starts_with_ci(line, "location")) {
            strlcpy(meta.location, value, sizeof(meta.location));
        }
    }
}

}  // namespace userspace::http
