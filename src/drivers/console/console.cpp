#include "console.hpp"

#include <stdarg.h>

#include "lib/mem.hpp"
#include "../../arch/x86_64/memory/paging.hpp"

namespace {

constexpr int kGlyphWidth = 8;
constexpr int kGlyphHeight = 8;
constexpr int kLineSpacing = 3 * scale;
constexpr uint8_t kMemoryModelRgb = 1;
constexpr size_t kPageSize = 0x1000;

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

inline void store_pixel(uint8_t* dst, size_t bpp, uint64_t packed_color) {
    for (size_t i = 0; i < bpp; ++i) {
        dst[i] = static_cast<uint8_t>((packed_color >> (8 * i)) & 0xFFu);
    }
}

void fill_span(uint8_t* dst,
               size_t pixel_count,
               size_t bpp,
               uint64_t packed_color) {
    if (dst == nullptr || pixel_count == 0 || bpp == 0) {
        return;
    }

    size_t total_bytes = pixel_count * bpp;
    store_pixel(dst, bpp, packed_color);
    size_t filled = bpp;

    while (filled < total_bytes) {
        size_t copy = (filled < total_bytes - filled) ? filled
                                                      : (total_bytes - filled);
        memcpy(dst + filled, dst, copy);
        filled += copy;
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
    if (width == 0 || height == 0) {
        return;
    }

    size_t bpp = bytes_per_pixel(fb);
    if (bpp == 0) {
        return;
    }

    if (x >= fb->width || y >= fb->height) {
        return;
    }

    if (width > fb->width - x) {
        width = fb->width - x;
    }

    uint64_t packed = pack_color(fb, color);
    for (size_t row = 0; row < height; ++row) {
        size_t py = y + row;
        if (py >= fb->height) {
            break;
        }

        uint8_t* dst = fb->base + py * fb->pitch + x * bpp;
        size_t max_bytes = fb->pitch - x * bpp;
        size_t row_pixels = width;
        size_t needed_bytes = row_pixels * bpp;
        if (needed_bytes > max_bytes) {
            row_pixels = max_bytes / bpp;
            if (row_pixels == 0) {
                continue;
            }
        }

        fill_span(dst, row_pixels, bpp, packed);
    }
}

}  // namespace

Console* kconsole = nullptr;
uint32_t DEFAULT_BG = 0x00000000;

bool Console::allocate_back_buffer() {
    if (back_buffer != nullptr) {
        return true;
    }
    if (primary_fb.base == nullptr || frame_bytes == 0) {
        return false;
    }

    size_t pages = (frame_bytes + kPageSize - 1) / kPageSize;
    uint8_t* start = nullptr;

    for (size_t i = 0; i < pages; ++i) {
        auto* page = static_cast<uint8_t*>(paging_alloc_page());
        if (page == nullptr) {
            return false;
        }
        if (i == 0) {
            start = page;
        }
    }

    back_buffer = start;
    back_buffer_capacity = pages * kPageSize;
    back_fb = primary_fb;
    back_fb.base = back_buffer;
    return true;
}

Framebuffer* Console::draw_target() {
    if (back_buffer != nullptr) {
        return &back_fb;
    }
    return &primary_fb;
}

bool Console::enable_back_buffer() {
    if (back_buffer != nullptr) {
        return true;
    }
    if (!allocate_back_buffer()) {
        return false;
    }
    if (frame_bytes == 0 || primary_fb.base == nullptr) {
        return true;
    }

    size_t bytes = frame_bytes;
    if (bytes > back_buffer_capacity) {
        bytes = back_buffer_capacity;
    }
    if (bytes == 0) {
        return true;
    }

    memcpy(back_buffer, primary_fb.base, bytes);
    return true;
}

void Console::flush_region(size_t x, size_t y, size_t width, size_t height) {
    if (back_buffer == nullptr || primary_fb.base == nullptr) {
        return;
    }
    if (width == 0 || height == 0) {
        return;
    }
    if (x >= primary_fb.width || y >= primary_fb.height) {
        return;
    }

    size_t bpp = bytes_per_pixel(&primary_fb);
    if (bpp == 0) {
        return;
    }

    size_t max_width = primary_fb.width - x;
    size_t copy_width = width > max_width ? max_width : width;
    size_t max_height = primary_fb.height - y;
    size_t copy_height = height > max_height ? max_height : height;

    size_t row_bytes = copy_width * bpp;

    for (size_t row = 0; row < copy_height; ++row) {
        size_t offset = (y + row) * primary_fb.pitch + x * bpp;
        if (offset >= frame_bytes || offset >= back_buffer_capacity) {
            break;
        }

        size_t usable = frame_bytes - offset;
        size_t cap_remaining = back_buffer_capacity - offset;
        if (usable > cap_remaining) {
            usable = cap_remaining;
        }

        size_t to_copy = row_bytes;
        if (to_copy > usable) {
            to_copy = usable;
        }
        if (to_copy == 0) {
            break;
        }

        memcpy_fast(primary_fb.base + offset, back_buffer + offset, to_copy);
    }
}

void Console::flush_all() {
    if (back_buffer == nullptr || primary_fb.base == nullptr) {
        return;
    }

    if (frame_bytes == 0 || back_buffer_capacity == 0) {
        return;
    }

    size_t bytes = frame_bytes;
    if (bytes > back_buffer_capacity) {
        bytes = back_buffer_capacity;
    }
    if (bytes == 0) {
        return;
    }

    memcpy_fast(primary_fb.base, back_buffer, bytes);
}

bool Console::refresh_framebuffer_info() {
    descriptor_defs::FramebufferInfo info{};
    int result = descriptor::get_property_kernel(
        framebuffer_handle,
        static_cast<uint32_t>(descriptor_defs::Property::FramebufferInfo),
        &info,
        sizeof(info));
    if (result != 0) {
        primary_fb = {};
        frame_bytes = 0;
        return false;
    }

    primary_fb.base = reinterpret_cast<uint8_t*>(info.virtual_base);
    primary_fb.width = static_cast<size_t>(info.width);
    primary_fb.height = static_cast<size_t>(info.height);
    primary_fb.pitch = static_cast<size_t>(info.pitch);
    primary_fb.bpp = info.bpp;
    primary_fb.memory_model = info.memory_model;
    primary_fb.red_mask_size = info.red_mask_size;
    primary_fb.red_mask_shift = info.red_mask_shift;
    primary_fb.green_mask_size = info.green_mask_size;
    primary_fb.green_mask_shift = info.green_mask_shift;
    primary_fb.blue_mask_size = info.blue_mask_size;
    primary_fb.blue_mask_shift = info.blue_mask_shift;

    frame_bytes = (primary_fb.pitch != 0)
                      ? primary_fb.pitch * primary_fb.height
                      : 0;
    return primary_fb.base != nullptr;
}

Console::Console(uint32_t framebuffer_handle)
    : framebuffer_handle(framebuffer_handle),
      primary_fb{},
      cursor_x(0),
      cursor_y(0),
      fg_color(0xFFFFFFFF),
      bg_color(0x00000000),
      columns(0),
      rows(0),
      text_width(0),
      text_height(0),
      back_fb{},
      back_buffer(nullptr),
      frame_bytes(0),
      back_buffer_capacity(0) {  // white on black
    refresh_framebuffer_info();

    size_t cw = cell_width_px();
    size_t ch = cell_height_px();
    if (cw == 0) {
        cw = 1;
    }
    if (ch == 0) {
        ch = 1;
    }
    columns = (primary_fb.width != 0) ? (primary_fb.width / cw) : 0;
    rows = (primary_fb.height != 0) ? (primary_fb.height / ch) : 0;
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

    if (frame_bytes == 0 && primary_fb.pitch != 0) {
        frame_bytes = primary_fb.pitch * primary_fb.height;
    }
}

void Console::draw_char(char c, size_t x, size_t y) {
    Framebuffer* target = draw_target();
    if (target == nullptr || target->base == nullptr) {
        return;
    }
    if (x >= columns || y >= rows) {
        return;
    }
    uint8_t uc = static_cast<uint8_t>(c);
    if (uc >= 128) return;

    size_t glyph_width = cell_width_px();
    size_t glyph_height = static_cast<size_t>(kGlyphHeight * scale);
    size_t base_px = x * glyph_width;
    size_t base_py = y * cell_height_px();
    if (base_px >= text_width || base_py >= target->height) {
        return;
    }

    size_t bpp = bytes_per_pixel(target);
    if (bpp == 0) {
        return;
    }

    size_t max_draw_width = text_width - base_px;
    if (max_draw_width == 0) {
        return;
    }

    size_t glyph_draw_width = glyph_width;
    if (glyph_draw_width > max_draw_width) {
        glyph_draw_width = max_draw_width;
    }
    if (glyph_draw_width == 0) {
        return;
    }

    uint64_t packed_fg = pack_color(target, fg_color);
    uint64_t packed_bg = pack_color(target, bg_color);

    for (int row = 0; row < kGlyphHeight; ++row) {
        uint8_t bits = font8x8_basic[uc][row];
        for (int dy = 0; dy < scale; ++dy) {
            size_t py = base_py + static_cast<size_t>(row) * scale + dy;
            if (py >= target->height) {
                continue;
            }

            uint8_t* dst = target->base + py * target->pitch + base_px * bpp;
            size_t px_offset = 0;
            for (int col = 0; col < kGlyphWidth && px_offset < glyph_draw_width; ++col) {
                bool bit_set = (bits & (1u << col)) != 0;
                size_t span = static_cast<size_t>(scale);
                if (px_offset + span > glyph_draw_width) {
                    span = glyph_draw_width - px_offset;
                }
                fill_span(dst + px_offset * bpp,
                          span,
                          bpp,
                          bit_set ? packed_fg : packed_bg);
                px_offset += span;
            }
            if (px_offset < glyph_draw_width) {
                fill_span(dst + px_offset * bpp,
                          glyph_draw_width - px_offset,
                          bpp,
                          packed_bg);
            }
        }
    }

    // fill line spacing with background colour
    size_t gap_start_y = base_py + glyph_height;
    if (kLineSpacing > 0 && gap_start_y < target->height && glyph_draw_width > 0) {
        fill_rect(target,
                  base_px,
                  gap_start_y,
                  glyph_draw_width,
                  static_cast<size_t>(kLineSpacing),
                  bg_color);
    }

    if (back_buffer != nullptr) {
        size_t flush_height = glyph_height + kLineSpacing;
        size_t remaining_height = (base_py < target->height)
                                      ? (target->height - base_py)
                                      : 0;
        if (flush_height > remaining_height) {
            flush_height = remaining_height;
        }
        if (flush_height == 0) {
            flush_height = glyph_height;
        }
        size_t flush_width = glyph_draw_width;
        if (flush_width == 0) {
            flush_width = glyph_width;
        }
        flush_region(base_px, base_py, flush_width, flush_height);
    }
}

void Console::set_color(uint32_t fg, uint32_t bg) {
    fg_color = fg;
    bg_color = bg;
}

void Console::scroll() {
    size_t row_height = cell_height_px();
    Framebuffer* target = draw_target();
    if (row_height == 0 || target == nullptr || target->base == nullptr) {
        return;
    }
    if (text_height == 0) {
        text_height = target->height - (target->height % row_height);
    }
    if (row_height >= text_height) {
        fill_rect(target, 0, 0, target->width, target->height, bg_color);
        if (back_buffer != nullptr) {
            flush_all();
        }
        cursor_y = 0;
        return;
    }

    size_t rows_to_copy = text_height - row_height;
    size_t bytes_to_copy = rows_to_copy * target->pitch;
    if (bytes_to_copy > 0) {
        memmove_fast(target->base,
                     target->base + row_height * target->pitch,
                     bytes_to_copy);
    }

    size_t copy_width = text_width > 0 ? text_width : target->width;
    if (copy_width > target->width) {
        copy_width = target->width;
    }

    fill_rect(target,
              0,
              text_height - row_height,
              copy_width,
              row_height,
              bg_color);

    if (cursor_y > 0) cursor_y--;

    if (back_buffer != nullptr) {
        flush_all();
    }
}

void Console::putc(char c) {
    Framebuffer* target = draw_target();
    if (c == '\n') {
        cursor_x = 0;
        cursor_y++;
        if (target != nullptr &&
            (cursor_y + 1) * cell_height_px() >= target->height) {
            scroll();
        }
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
            size_t max_cols = target != nullptr ? (target->width / cell_width_px())
                                                : 0;
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
    Framebuffer* target = draw_target();
    if (target == nullptr || target->base == nullptr) {
        return;
    }
    fill_rect(target, 0, 0, target->width, target->height, bg_color);
    if (back_buffer != nullptr) {
        flush_all();
    }
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
