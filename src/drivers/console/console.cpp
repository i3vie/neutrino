#include "console.hpp"

#include <stdarg.h>

Console* kconsole = nullptr;
uint32_t DEFAULT_BG = 0x00000000;

Console::Console(Framebuffer* fb)
    : fb(fb),
      cursor_x(0),
      cursor_y(0),
      fg_color(0xFFFFFFFF),
      bg_color(0x00000000) {}  // white on black

void Console::draw_char(char c, size_t x, size_t y) {
    if ((unsigned char)c >= 128) return;

    for (int row = 0; row < 8; ++row) {
        uint8_t bits = font8x8_basic[(uint8_t)c][row];
        for (int col = 0; col < 8; ++col) {
            uint32_t color = (bits & (1 << col)) ? fg_color : bg_color;
            size_t base_px = x * 8 * scale + col * scale;
            size_t base_py = y * 8 * scale + row * scale;

            for (int dy = 0; dy < scale; ++dy) {
                for (int dx = 0; dx < scale; ++dx) {
                    size_t px = base_px + dx;
                    size_t py = base_py + dy;
                    if (px < fb->width && py < fb->height) {
                        ((uint32_t*)fb->base)[py * (fb->pitch / 4) + px] =
                            color;
                    }
                }
            }
        }
    }
}

void Console::set_color(uint32_t fg, uint32_t bg = DEFAULT_BG) {
    fg_color = fg;
    bg_color = bg;
}

void Console::scroll() {
    size_t row_bytes = 8 * scale * fb->pitch;
    uint8_t* dst = fb->base;
    uint8_t* src = fb->base + row_bytes;
    size_t copy_bytes = (fb->height * fb->pitch) - row_bytes;

    for (size_t i = 0; i < copy_bytes; i++) dst[i] = src[i];

    // clear last line
    for (size_t i = copy_bytes; i < fb->height * fb->pitch; i++)
        fb->base[i] = 0;

    if (cursor_y > 0) cursor_y--;
}

void Console::putc(char c) {
    if (c == '\n') {
        cursor_x = 0;
        cursor_y++;
        if ((cursor_y + 1) * 8 * scale >= fb->height) scroll();
        return;
    }
    if (c == '\r') {
        cursor_x = 0;
        return;
    }
    if (c == '\b') {
        if (cursor_x > 0) {
            cursor_x--;
        } else if (cursor_y > 0) {
            size_t max_cols = fb->width / (8 * scale);
            if (max_cols > 0) {
                cursor_y--;
                cursor_x = max_cols - 1;
            }
        }
        draw_char(' ', cursor_x, cursor_y);
        return;
    }

    draw_char(c, cursor_x, cursor_y);
    cursor_x++;
    if ((cursor_x + 1) * 8 * scale >= fb->width) {
        cursor_x = 0;
        cursor_y++;
        if ((cursor_y + 1) * 8 * scale >= fb->height) scroll();
    }
}

void Console::puts(const char* s) {
    while (*s) putc(*s++);
}

void Console::clear() {
    for (size_t i = 0; i < fb->height * fb->pitch; i++) fb->base[i] = 0;
    cursor_x = cursor_y = 0;
}

void Console::print_dec(uint64_t n) {
    char buf[21];
    int i = 0;
    if (n == 0) {
        putc('0');
        return;
    }
    while (n > 0) {
        buf[i++] = '0' + (n % 10);
        n /= 10;
    }
    while (i--) putc(buf[i]);
}

void Console::print_hex(uint64_t n, bool pad16) {
    char buf[17];
    int i = 0;

    // handle zero explicitly
    if (n == 0) {
        if (pad16)
            puts("0x0000000000000000");
        else
            puts("0x0");
        return;
    }

    // convert to hex string (reversed)
    while (n > 0 && i < 16) {
        uint8_t d = n & 0xF;
        buf[i++] = (d < 10) ? ('0' + d) : ('a' + (d - 10));
        n >>= 4;
    }

    puts("0x");

    // pad to 16 digits if requested
    if (pad16 && i < 16) {
        for (int j = 0; j < 16 - i; j++)
            putc('0');
    }

    // print reversed buffer
    while (i--)
        putc(buf[i]);
}


void Console::printf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    while (*fmt) {
        if (*fmt != '%') {
            putc(*fmt++);
            continue;
        }
        fmt++;

        bool pad16 = false;

        // check for "%016x"
        if (*fmt == '0' && *(fmt + 1) == '1' && *(fmt + 2) == '6') {
            fmt += 3;
            if (*fmt == 'x') {
                pad16 = true;
            }
        }

        switch (*fmt++) {
            case 'd':
                print_dec(va_arg(args, int));
                break;
            case 'x':
                print_hex(va_arg(args, unsigned long long), pad16);
                break;
            case 's':
                puts(va_arg(args, const char*));
                break;
            case 'c':
                putc((char)va_arg(args, int));
                break;
            case '%':
                putc('%');
                break;
            default:
                putc('?');
                break;
        }
    }
    va_end(args);
}
