#include "console.hpp"

#include <stdarg.h>

namespace {

constexpr int kGlyphWidth = 8;
constexpr int kGlyphHeight = 8;
constexpr int kLineSpacing = 5;

size_t cell_width_px() {
    return static_cast<size_t>(kGlyphWidth * scale);
}

size_t cell_height_px() {
    return static_cast<size_t>(kGlyphHeight * scale + kLineSpacing);
}

void fill_rect(Framebuffer* fb,
               size_t x,
               size_t y,
               size_t width,
               size_t height,
               uint32_t color) {
    if (fb == nullptr || fb->base == nullptr) {
        return;
    }
    for (size_t row = 0; row < height; ++row) {
        size_t py = y + row;
        if (py >= fb->height) {
            break;
        }
        uint32_t* dst =
            reinterpret_cast<uint32_t*>(fb->base + py * fb->pitch);
        for (size_t col = 0; col < width; ++col) {
            size_t px = x + col;
            if (px >= fb->width) {
                break;
            }
            dst[px] = color;
        }
    }
}

}  // namespace

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

    size_t glyph_width = cell_width_px();
    size_t glyph_height = static_cast<size_t>(kGlyphHeight * scale);
    size_t base_px = x * glyph_width;
    size_t base_py = y * cell_height_px();

    for (int row = 0; row < kGlyphHeight; ++row) {
        uint8_t bits = font8x8_basic[(uint8_t)c][row];
        for (int col = 0; col < kGlyphWidth; ++col) {
            uint32_t color = (bits & (1 << col)) ? fg_color : bg_color;
            for (int dy = 0; dy < scale; ++dy) {
                for (int dx = 0; dx < scale; ++dx) {
                    size_t px = base_px + col * scale + dx;
                    size_t py = base_py + row * scale + dy;
                    if (px < fb->width && py < fb->height) {
                        ((uint32_t*)fb->base)[py * (fb->pitch / 4) + px] =
                            color;
                    }
                }
            }
        }
    }

    // fill line spacing with background colour
    size_t gap_start_y = base_py + glyph_height;
    if (kLineSpacing > 0 && gap_start_y < fb->height) {
        fill_rect(fb,
                  base_px,
                  gap_start_y,
                  glyph_width,
                  static_cast<size_t>(kLineSpacing),
                  bg_color);
    }
}

void Console::set_color(uint32_t fg, uint32_t bg = DEFAULT_BG) {
    fg_color = fg;
    bg_color = bg;
}

void Console::scroll() {
    size_t row_bytes = cell_height_px() * fb->pitch;
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
        if ((cursor_y + 1) * cell_height_px() >= fb->height) scroll();
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
            size_t max_cols = fb->width / cell_width_px();
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
    if ((cursor_x + 1) * cell_width_px() >= fb->width) {
        cursor_x = 0;
        cursor_y++;
        if ((cursor_y + 1) * cell_height_px() >= fb->height) scroll();
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
