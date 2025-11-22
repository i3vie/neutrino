#include "logging.hpp"

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "drivers/console/console.hpp"
#include "../serial/serial.hpp"
#include "lib/mem.hpp"

namespace {

constexpr size_t LOG_BUFFER_CAPACITY = 32 * 1024;
constexpr size_t LOG_LINE_MAX = 512;

char g_buffer[LOG_BUFFER_CAPACITY];
volatile int g_console_lock = 0;
size_t g_write_pos = 0;
size_t g_start_pos = 0;
size_t g_size = 0;
bool g_initialized = false;

const char* level_tag(LogLevel level) {
    switch (level) {
        case LogLevel::Debug:
            return "DEBUG";
        case LogLevel::Info:
            return "INFO";
        case LogLevel::Warn:
            return "WARN";
        case LogLevel::Error:
        default:
            return "ERROR";
    }
}

uint32_t level_color(LogLevel level) {
    switch (level) {
        case LogLevel::Debug:
            return 0xFF5FA8FF;
        case LogLevel::Info:
            return 0xFFFFFFFF;
        case LogLevel::Warn:
            return 0xFFFFD37F;
        case LogLevel::Error:
        default:
            return 0xFFFF6060;
    }
}

void lock_console() {
    while (__atomic_test_and_set(&g_console_lock, __ATOMIC_ACQUIRE)) {
        asm volatile("pause");
    }
}

void unlock_console() {
    __atomic_clear(&g_console_lock, __ATOMIC_RELEASE);
}

void push_char(char c) {
    g_buffer[g_write_pos] = c;
    g_write_pos = (g_write_pos + 1) % LOG_BUFFER_CAPACITY;
    if (g_size < LOG_BUFFER_CAPACITY) {
        ++g_size;
    } else {
        g_start_pos = (g_start_pos + 1) % LOG_BUFFER_CAPACITY;
    }
}

void append_line_to_buffer(const char* line) {
    if (!line) return;

    size_t len = 0;
    while (line[len] != '\0' && len < LOG_LINE_MAX - 1) {
        ++len;
    }

    if (len >= LOG_BUFFER_CAPACITY) {
        line += len - (LOG_BUFFER_CAPACITY - 1);
        len = LOG_BUFFER_CAPACITY - 1;
    }

    for (size_t i = 0; i < len; ++i) {
        push_char(line[i]);
    }
    push_char('\n');
}

void append_char(char*& out, char* end, char c) {
    if (out < end) {
        *out = c;
    }
    ++out;
}

void append_string(char*& out, char* end, const char* str) {
    if (!str) return;
    while (*str) {
        append_char(out, end, *str++);
    }
}

void append_unsigned(char*& out, char* end, uint64_t value, int width,
                     char pad_char) {
    char buffer[21];
    int pos = 0;
    if (value == 0) {
        buffer[pos++] = '0';
    } else {
        while (value > 0 && pos < 21) {
            buffer[pos++] = static_cast<char>('0' + (value % 10));
            value /= 10;
        }
    }

    int total_len = pos;
    int pad_len = width > total_len ? width - total_len : 0;
    while (pad_len-- > 0) {
        append_char(out, end, pad_char);
    }
    while (pos--) {
        append_char(out, end, buffer[pos]);
    }
}

void append_signed(char*& out, char* end, int64_t value, int width,
                   char pad_char) {
    bool negative = value < 0;
    uint64_t mag = negative ? static_cast<uint64_t>(-value)
                            : static_cast<uint64_t>(value);

    char buffer[21];
    int pos = 0;
    if (mag == 0) {
        buffer[pos++] = '0';
    } else {
        while (mag > 0 && pos < 21) {
            buffer[pos++] = static_cast<char>('0' + (mag % 10));
            mag /= 10;
        }
    }

    int total_len = pos + (negative ? 1 : 0);
    int pad_len = width > total_len ? width - total_len : 0;

    if (negative && pad_char == '0') {
        append_char(out, end, '-');
        while (pad_len-- > 0) {
            append_char(out, end, '0');
        }
    } else {
        while (pad_len-- > 0) {
            append_char(out, end, pad_char);
        }
        if (negative) {
            append_char(out, end, '-');
        }
    }

    while (pos--) {
        append_char(out, end, buffer[pos]);
    }
}

void append_hex(char*& out, char* end, uint64_t value, bool pad16) {
    char buffer[16];
    int pos = 0;
    if (value == 0) {
        if (pad16) {
            append_string(out, end, "0x0000000000000000");
        } else {
            append_string(out, end, "0x0");
        }
        return;
    }
    while (value > 0 && pos < 16) {
        uint8_t digit = static_cast<uint8_t>(value & 0xF);
        buffer[pos++] = (digit < 10) ? static_cast<char>('0' + digit)
                                     : static_cast<char>('a' + (digit - 10));
        value >>= 4;
    }
    append_string(out, end, "0x");
    if (pad16 && pos < 16) {
        for (int i = 0; i < 16 - pos; ++i) {
            append_char(out, end, '0');
        }
    }
    while (pos--) {
        append_char(out, end, buffer[pos]);
    }
}

size_t format_message(char* out, size_t capacity, const char* fmt,
                      va_list args_origin) {
    if (capacity == 0) return 0;
    char* cursor = out;
    char* end = out + capacity - 1;

    va_list args;
    va_copy(args, args_origin);

    while (*fmt) {
        if (*fmt != '%') {
            append_char(cursor, end, *fmt++);
            continue;
        }
        ++fmt;

        bool zero_pad = false;
        int width = 0;
        if (*fmt == '0') {
            zero_pad = true;
            ++fmt;
        }
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            ++fmt;
        }

        char length = '\0';
        if (*fmt == 'z' || *fmt == 'l') {
            length = *fmt++;
            if (length == 'l' && *fmt == 'l') {
                length = 'L';
                ++fmt;
            }
        }

        char spec = *fmt++;
        bool pad16 = false;
        if (zero_pad && width == 16 && spec == 'x') {
            pad16 = true;
            width = 0;  // handled separately
        }
        switch (spec) {
            case 's':
                append_string(cursor, end, va_arg(args, const char*));
                break;
            case 'd':
            case 'i': {
                long long value = 0;
                switch (length) {
                    case 'z':
                        value = static_cast<long long>(va_arg(args, long));
                        break;
                    case 'l':
                        value = va_arg(args, long);
                        break;
                    case 'L':
                        value = va_arg(args, long long);
                        break;
                    default:
                        value = va_arg(args, int);
                        break;
                }
                append_signed(cursor, end, value, width,
                              zero_pad ? '0' : ' ');
                break;
            }
            case 'u': {
                uint64_t value = 0;
                switch (length) {
                    case 'z':
                        value = static_cast<uint64_t>(va_arg(args, size_t));
                        break;
                    case 'l':
                        value = static_cast<uint64_t>(va_arg(
                            args, unsigned long));
                        break;
                    case 'L':
                        value = va_arg(args, unsigned long long);
                        break;
                    default:
                        value =
                            static_cast<uint64_t>(va_arg(args, unsigned int));
                        break;
                }
                append_unsigned(cursor, end, value, width,
                                zero_pad ? '0' : ' ');
                break;
            }
            case 'x': {
                uint64_t value = 0;
                switch (length) {
                    case 'z':
                        value = static_cast<uint64_t>(va_arg(args, size_t));
                        break;
                    case 'l':
                        value = static_cast<uint64_t>(va_arg(
                            args, unsigned long));
                        break;
                    case 'L':
                        value = va_arg(args, unsigned long long);
                        break;
                    default:
                        value = va_arg(args, unsigned long long);
                        break;
                }
                append_hex(cursor, end, value, pad16);
                break;
            }
            case 'p':
                append_hex(cursor, end, va_arg(args, unsigned long long), true);
                break;
            case 'c':
                append_char(cursor, end, static_cast<char>(va_arg(args, int)));
                break;
            case '%':
                append_char(cursor, end, '%');
                break;
            default:
                append_char(cursor, end, '%');
                append_char(cursor, end, spec);
                break;
        }
    }

    va_end(args);

    if (cursor <= end) {
        *cursor = '\0';
    } else {
        out[capacity - 1] = '\0';
    }

    return static_cast<size_t>(cursor - out);
}

void store_log_line(const char* tag, const char* message) {
    char line[LOG_LINE_MAX];
    char* cursor = line;
    char* end = line + sizeof(line) - 1;

    append_char(cursor, end, '[');
    append_string(cursor, end, tag);
    append_string(cursor, end, "] ");
    append_string(cursor, end, message);

    if (cursor <= end) {
        *cursor = '\0';
    } else {
        line[sizeof(line) - 1] = '\0';
    }

    append_line_to_buffer(line);
}

void emit_to_console(LogLevel level, const char* tag, const char* message) {
    if (kconsole == nullptr) {
        return;
    }

    uint32_t fg = level_color(level);
    kconsole->set_color(fg, 0x00000000);
    kconsole->printf("[%s] %s\n", tag, message);
    kconsole->set_color(0xFFFFFFFF, 0x00000000);
}

void emit_to_serial(const char* tag, const char* message) {
    serial::write_string("[");
    serial::write_string(tag);
    serial::write_string("] ");
    serial::write_string(message);
    serial::write_string("\n");
}

}  // namespace

void log_init() {
    serial::init();
    memset(g_buffer, 0, sizeof(g_buffer));
    g_write_pos = 0;
    g_start_pos = 0;
    g_size = 0;
    g_initialized = true;
}

void log_message(LogLevel level, const char* fmt, ...) {
    if (!g_initialized) {
        log_init();
    }

    char buffer[256];
    va_list args;
    va_start(args, fmt);
    format_message(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    const char* tag = level_tag(level);

    lock_console();
    emit_to_serial(tag, buffer);
    emit_to_console(level, tag, buffer);
    store_log_line(tag, buffer);
    unlock_console();
}

size_t log_copy_recent(char* out, size_t max_len) {
    if (out == nullptr || max_len == 0) {
        return 0;
    }

    if (max_len == 1) {
        out[0] = '\0';
        return 0;
    }

    size_t available = g_size;
    if (available == 0) {
        out[0] = '\0';
        return 0;
    }

    size_t to_copy = (available < max_len - 1) ? available : (max_len - 1);
    size_t src = g_start_pos;

    for (size_t i = 0; i < to_copy; ++i) {
        out[i] = g_buffer[src];
        src = (src + 1) % LOG_BUFFER_CAPACITY;
    }
    out[to_copy] = '\0';
    return to_copy;
}
