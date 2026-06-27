#pragma once

#include <stddef.h>
#include <stdint.h>

namespace userspace::http {

constexpr size_t kMaxUrl = 512;
constexpr size_t kMaxHost = 256;
constexpr size_t kMaxPath = 512;
constexpr size_t kMaxLocation = 512;

enum class Scheme : uint8_t {
    Http,
    Https,
};

enum class UrlParseMode : uint8_t {
    RequireScheme,
    DefaultHttps,
    DefaultHttp,
};

struct Url {
    Scheme scheme;
    char host[kMaxHost];
    char path[kMaxPath];
    uint16_t port;
};

struct ResponseMeta {
    int status_code;
    bool chunked;
    bool have_content_length;
    size_t content_length;
    bool is_html;
    bool is_text;
    char location[kMaxLocation];
};

using ReadLineFn = bool (*)(void* context, char* out, size_t out_size);

bool is_digit(char ch);
char to_lower(char ch);
bool starts_with_ci(const char* text, const char* prefix);
int find_char(const char* text, char ch);
void copy_range(char* dest, size_t capacity, const char* begin, const char* end);
bool append_cstr(char* dest, size_t capacity, const char* src);
void trim_spaces(char* text);

bool parse_u16_range(const char* begin, const char* end, uint16_t& out);
bool parse_decimal(const char* text, size_t& out);
int parse_decimal_int(const char* text);
bool parse_decimal_range(const char* begin, const char* end, int& out);
bool parse_hex_size(const char* text, size_t& out);

bool parse_url(const char* text, Url& out, UrlParseMode mode);
bool parse_ipv4_literal(const char* text, uint8_t out[4]);
bool url_to_string(const Url& url, char* out, size_t out_size);
bool build_redirect_url(const Url& current,
                        const char* location,
                        char* out,
                        size_t out_size);
void init_response_meta(ResponseMeta& meta);
bool read_response_headers(void* context, ReadLineFn read_line, ResponseMeta& meta);

}  // namespace userspace::http
