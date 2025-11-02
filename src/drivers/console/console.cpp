#include "console.hpp"

#include <stdarg.h>

#include "lib/mem.hpp"

namespace {

constexpr int kGlyphWidth = 8;
constexpr int kGlyphHeight = 8;
constexpr int kLineSpacing = 3 * scale;
constexpr uint8_t kMemoryModelRgb = 1;

size_t bytes_per_pixel(const Framebuffer* fb) {
    if (fb == nullptr || fb->bpp == 0) {
        return 4;
    }
    return static_cast<size_t>((fb->bpp + 7) / 8);
}

uint32_t scale_component(uint8_t value, uint8_t bits) {
    if (bits == 0) {
        return 0;
    }
    if (bits >= 8) {
        return static_cast<uint32_t>(value) << (bits - 8);
    }
    uint32_t max_value = (1u << bits) - 1u;
    return (static_cast<uint32_t>(value) * max_value + 127u) / 255u;
}

uint64_t pack_color(const Framebuffer* fb, uint32_t argb) {
    if (fb == nullptr) {
        return argb;
    }

    if (fb->memory_model != kMemoryModelRgb) {
        return argb;
    }

    uint8_t red = static_cast<uint8_t>((argb >> 16) & 0xFF);
    uint8_t green = static_cast<uint8_t>((argb >> 8) & 0xFF);
    uint8_t blue = static_cast<uint8_t>(argb & 0xFF);

    uint64_t packed = 0;
    packed |= static_cast<uint64_t>(
                  scale_component(red, fb->red_mask_size))
              << fb->red_mask_shift;
    packed |= static_cast<uint64_t>(
                  scale_component(green, fb->green_mask_size))
              << fb->green_mask_shift;
    packed |= static_cast<uint64_t>(
                  scale_component(blue, fb->blue_mask_size))
              << fb->blue_mask_shift;
    return packed;
}

void write_packed_pixel(Framebuffer* fb,
                        size_t px,
                        size_t py,
                        uint64_t packed_color) {
    if (fb == nullptr || fb->base == nullptr) {
        return;
    }
    if (px >= fb->width || py >= fb->height) {
        return;
    }

    size_t bpp_bytes = bytes_per_pixel(fb);
    uint8_t* dst =
        fb->base + py * fb->pitch + px * bpp_bytes;
    for (size_t i = 0; i < bpp_bytes; ++i) {
        dst[i] = static_cast<uint8_t>(
            (packed_color >> (8 * i)) & 0xFF);
    }
}

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
    uint64_t packed = pack_color(fb, color);
    for (size_t row = 0; row < height; ++row) {
        size_t py = y + row;
        if (py >= fb->height) {
            break;
        }
        for (size_t col = 0; col < width; ++col) {
            size_t px = x + col;
            if (px >= fb->width) {
                break;
            }
            write_packed_pixel(fb, px, py, packed);
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
      bg_color(0x00000000),
      columns(0),
      rows(0),
      text_width(0),
      text_height(0) {  // white on black
    size_t cw = cell_width_px();
    size_t ch = cell_height_px();
    if (cw == 0) {
        cw = 1;
    }
    if (ch == 0) {
        ch = 1;
    }
    columns = fb ? (fb->width / cw) : 0;
    rows = fb ? (fb->height / ch) : 0;
    text_width = columns * cw;
    text_height = rows * ch;
    
    if (columns == 0) {
        columns = 1;
        text_width = cw;
    }
    if (rows == 0) {
        rows = 1;
        text_height = ch;
    }
}

void Console::draw_char(char c, size_t x, size_t y) {
    if (x >= columns || y >= rows) {
        return;
    }
    if ((unsigned char)c >= 128) return;

    size_t glyph_width = cell_width_px();
    size_t glyph_height = static_cast<size_t>(kGlyphHeight * scale);
    size_t base_px = x * glyph_width;
    size_t base_py = y * cell_height_px();
    uint64_t packed_fg = pack_color(fb, fg_color);
    uint64_t packed_bg = pack_color(fb, bg_color);

    for (int row = 0; row < kGlyphHeight; ++row) {
        uint8_t bits = font8x8_basic[(uint8_t)c][row];
        for (int col = 0; col < kGlyphWidth; ++col) {
            uint64_t packed =
                (bits & (1 << col)) ? packed_fg : packed_bg;
            for (int dy = 0; dy < scale; ++dy) {
                for (int dx = 0; dx < scale; ++dx) {
                    size_t px = base_px + col * scale + dx;
                    size_t py = base_py + row * scale + dy;
                    if (px >= text_width) {
                        continue;
                    }
                    write_packed_pixel(fb, px, py, packed);
                }
            }
        }
    }

    // fill line spacing with background colour
    size_t gap_start_y = base_py + glyph_height;
    if (kLineSpacing > 0 && gap_start_y < fb->height) {
        size_t draw_width = glyph_width;
        if (base_px + draw_width > text_width) {
            if (base_px >= text_width) {
                draw_width = 0;
            } else {
                draw_width = text_width - base_px;
            }
        }
        if (draw_width == 0) {
            return;
        }
        fill_rect(fb,
                  base_px,
                  gap_start_y,
                  draw_width,
                  static_cast<size_t>(kLineSpacing),
                  bg_color);
    }
}

void Console::set_color(uint32_t fg, uint32_t bg = DEFAULT_BG) {
    fg_color = fg;
    bg_color = bg;
}

void Console::scroll() {
    size_t row_height = cell_height_px();
    if (row_height == 0 || fb == nullptr) {
        return;
    }
    if (text_height == 0) {
        text_height = fb->height - (fb->height % row_height);
    }
    if (row_height >= text_height) {
        fill_rect(fb, 0, 0, fb->width, fb->height, bg_color);
        cursor_y = 0;
        return;
    }

    size_t bpp = bytes_per_pixel(fb);
    size_t copy_width = text_width > 0 ? text_width : fb->width;
    size_t copy_bytes = copy_width * bpp;
    if (copy_bytes > fb->pitch) {
        copy_bytes = fb->pitch;
    }

    size_t total_rows = text_height - row_height;
    for (size_t py = 0; py < total_rows; ++py) {
        uint8_t* dst = fb->base + py * fb->pitch;
        const uint8_t* src = fb->base + (py + row_height) * fb->pitch;
        memcpy(dst, src, copy_bytes);
    }

    fill_rect(fb,
              0,
              text_height - row_height,
              copy_width,
              row_height,
              bg_color);

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
    if (cursor_x >= columns) {
        cursor_x = 0;
        cursor_y++;
        if (cursor_y >= rows) scroll();
    }
}

void Console::puts(const char* s) {
    while (*s) putc(*s++);
}

void Console::clear() {
    fill_rect(fb, 0, 0, fb->width, fb->height, bg_color);
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
