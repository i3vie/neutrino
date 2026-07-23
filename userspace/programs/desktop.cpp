#include <stddef.h>
#include <stdint.h>

#include "../crt/syscall.hpp"
#include "font8x8_basic.hpp"

namespace {

constexpr uint32_t kKeyboardType =
    static_cast<uint32_t>(descriptor_defs::Type::Keyboard);
constexpr int32_t kCursorWidth = 12;
constexpr int32_t kCursorHeight = 18;

descriptor_defs::FramebufferInfo g_fb{};
uint32_t g_framebuffer = kInvalidDescriptor;
uint8_t* g_pixels = nullptr;
uint32_t g_bytes_per_pixel = 0;
int32_t g_cursor_x = 0;
int32_t g_cursor_y = 0;
uint32_t g_cursor_backing[kCursorWidth * kCursorHeight]{};
bool g_cursor_saved = false;
bool g_menu_open = false;

uint32_t scale_channel(uint8_t value, uint8_t bits) {
    if (bits == 0) {
        return 0;
    }
    uint32_t maximum = bits >= 32 ? UINT32_MAX : ((1u << bits) - 1u);
    return static_cast<uint32_t>(
        (static_cast<uint64_t>(value) * maximum + 127u) / 255u);
}

uint32_t color(uint8_t red, uint8_t green, uint8_t blue) {
    return (scale_channel(red, g_fb.red_mask_size) << g_fb.red_mask_shift) |
           (scale_channel(green, g_fb.green_mask_size)
            << g_fb.green_mask_shift) |
           (scale_channel(blue, g_fb.blue_mask_size)
            << g_fb.blue_mask_shift);
}

bool inside(int32_t x, int32_t y) {
    return x >= 0 && y >= 0 &&
           x < static_cast<int32_t>(g_fb.width) &&
           y < static_cast<int32_t>(g_fb.height);
}

uint32_t read_pixel(int32_t x, int32_t y) {
    if (!inside(x, y)) {
        return 0;
    }
    const uint8_t* pixel =
        g_pixels + static_cast<size_t>(y) * g_fb.pitch +
        static_cast<size_t>(x) * g_bytes_per_pixel;
    uint32_t value = 0;
    for (uint32_t i = 0; i < g_bytes_per_pixel; ++i) {
        value |= static_cast<uint32_t>(pixel[i]) << (i * 8u);
    }
    return value;
}

void put_pixel(int32_t x, int32_t y, uint32_t value) {
    if (!inside(x, y)) {
        return;
    }
    uint8_t* pixel = g_pixels + static_cast<size_t>(y) * g_fb.pitch +
                     static_cast<size_t>(x) * g_bytes_per_pixel;
    for (uint32_t i = 0; i < g_bytes_per_pixel; ++i) {
        pixel[i] = static_cast<uint8_t>(value >> (i * 8u));
    }
}

void fill_rect(int32_t x,
               int32_t y,
               int32_t width,
               int32_t height,
               uint32_t value) {
    if (width <= 0 || height <= 0) {
        return;
    }
    int32_t end_x = x + width;
    int32_t end_y = y + height;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (end_x > static_cast<int32_t>(g_fb.width)) {
        end_x = static_cast<int32_t>(g_fb.width);
    }
    if (end_y > static_cast<int32_t>(g_fb.height)) {
        end_y = static_cast<int32_t>(g_fb.height);
    }
    for (int32_t row = y; row < end_y; ++row) {
        for (int32_t column = x; column < end_x; ++column) {
            put_pixel(column, row, value);
        }
    }
}

void draw_char(int32_t x,
               int32_t y,
               char character,
               uint32_t foreground,
               uint32_t scale = 1) {
    uint8_t code = static_cast<uint8_t>(character);
    if (code >= 128 || scale == 0) {
        return;
    }
    for (int32_t row = 0; row < 8; ++row) {
        uint8_t glyph_row = font8x8_basic[code][row];
        for (int32_t column = 0; column < 8; ++column) {
            if ((glyph_row & (1u << column)) == 0) {
                continue;
            }
            fill_rect(x + column * static_cast<int32_t>(scale),
                      y + row * static_cast<int32_t>(scale),
                      static_cast<int32_t>(scale),
                      static_cast<int32_t>(scale),
                      foreground);
        }
    }
}

void draw_text(int32_t x,
               int32_t y,
               const char* text,
               uint32_t foreground,
               uint32_t scale = 1) {
    if (text == nullptr) {
        return;
    }
    int32_t advance = static_cast<int32_t>(8u * scale);
    while (*text != '\0') {
        draw_char(x, y, *text++, foreground, scale);
        x += advance;
    }
}

void draw_frame(int32_t x,
                int32_t y,
                int32_t width,
                int32_t height,
                uint32_t value) {
    fill_rect(x, y, width, 1, value);
    fill_rect(x, y + height - 1, width, 1, value);
    fill_rect(x, y, 1, height, value);
    fill_rect(x + width - 1, y, 1, height, value);
}

void draw_desktop() {
    const uint32_t background = color(22, 68, 92);
    const uint32_t taskbar = color(19, 25, 34);
    const uint32_t panel = color(237, 241, 244);
    const uint32_t title = color(34, 96, 132);
    const uint32_t white = color(255, 255, 255);
    const uint32_t dark = color(25, 31, 38);
    const uint32_t border = color(11, 17, 22);
    const uint32_t accent = color(64, 169, 211);

    fill_rect(0, 0, static_cast<int32_t>(g_fb.width),
              static_cast<int32_t>(g_fb.height), background);

    int32_t window_width = static_cast<int32_t>(g_fb.width) * 3 / 5;
    int32_t window_height = static_cast<int32_t>(g_fb.height) / 2;
    if (window_width < 280) window_width = 280;
    if (window_height < 150) window_height = 150;
    int32_t window_x =
        (static_cast<int32_t>(g_fb.width) - window_width) / 2;
    int32_t window_y =
        (static_cast<int32_t>(g_fb.height) - window_height) / 2 - 12;
    fill_rect(window_x + 5, window_y + 6, window_width, window_height,
              color(12, 42, 57));
    fill_rect(window_x, window_y, window_width, window_height, panel);
    fill_rect(window_x, window_y, window_width, 30, title);
    draw_frame(window_x, window_y, window_width, window_height, border);
    draw_text(window_x + 10, window_y + 7, "Neutrino Desktop", white, 2);
    draw_text(window_x + 22, window_y + 58,
              "The graphical session is active.", dark, 1);
    draw_text(window_x + 22, window_y + 78,
              "Mouse input is routed through its seat.", dark, 1);
    draw_text(window_x + 22, window_y + 98,
              "Press Escape to return to the console.", dark, 1);
    fill_rect(window_x + 22, window_y + window_height - 38,
              window_width - 44, 2, accent);

    int32_t taskbar_y = static_cast<int32_t>(g_fb.height) - 42;
    fill_rect(0, taskbar_y, static_cast<int32_t>(g_fb.width), 42, taskbar);
    fill_rect(8, taskbar_y + 7, 92, 28, accent);
    draw_text(21, taskbar_y + 13, "NEUTRINO", dark, 1);
    draw_text(static_cast<int32_t>(g_fb.width) - 146,
              taskbar_y + 13, "SESSION 1", white, 1);

    if (g_menu_open) {
        int32_t menu_y = taskbar_y - 174;
        fill_rect(8, menu_y, 228, 166, panel);
        draw_frame(8, menu_y, 228, 166, border);
        fill_rect(9, menu_y + 1, 226, 34, title);
        draw_text(21, menu_y + 10, "Neutrino", white, 2);
        draw_text(24, menu_y + 55, "Applications", dark, 1);
        draw_text(24, menu_y + 81, "Files", dark, 1);
        draw_text(24, menu_y + 107, "Settings", dark, 1);
        draw_text(24, menu_y + 137, "Escape: close desktop", dark, 1);
    }

    (void)framebuffer_present(g_framebuffer, nullptr);
}

bool cursor_pixel(int32_t x, int32_t y) {
    if (x == 0 && y < 15) return true;
    if (y == x * 2 && y < 16) return true;
    if (y == x * 2 + 1 && y < 16) return true;
    if (y >= 10 && y <= 16 && x >= 3 && x <= 5) return true;
    if (y >= 14 && y <= 17 && x >= 6 && x <= 8) return true;
    return false;
}

void restore_cursor() {
    if (!g_cursor_saved) {
        return;
    }
    for (int32_t y = 0; y < kCursorHeight; ++y) {
        for (int32_t x = 0; x < kCursorWidth; ++x) {
            put_pixel(g_cursor_x + x, g_cursor_y + y,
                      g_cursor_backing[y * kCursorWidth + x]);
        }
    }
    g_cursor_saved = false;
}

void draw_cursor() {
    const uint32_t outline = color(0, 0, 0);
    const uint32_t fill = color(255, 255, 255);
    for (int32_t y = 0; y < kCursorHeight; ++y) {
        for (int32_t x = 0; x < kCursorWidth; ++x) {
            g_cursor_backing[y * kCursorWidth + x] =
                read_pixel(g_cursor_x + x, g_cursor_y + y);
        }
    }
    g_cursor_saved = true;
    for (int32_t y = 0; y < kCursorHeight; ++y) {
        for (int32_t x = 0; x < kCursorWidth; ++x) {
            if (!cursor_pixel(x, y)) {
                continue;
            }
            bool edge = x == 0 || y == x * 2 || y == x * 2 + 1 ||
                        y == kCursorHeight - 1;
            put_pixel(g_cursor_x + x, g_cursor_y + y,
                      edge ? outline : fill);
        }
    }
}

void clamp_cursor() {
    if (g_cursor_x < 0) g_cursor_x = 0;
    if (g_cursor_y < 0) g_cursor_y = 0;
    int32_t max_x = static_cast<int32_t>(g_fb.width) - kCursorWidth;
    int32_t max_y = static_cast<int32_t>(g_fb.height) - kCursorHeight;
    if (g_cursor_x > max_x) g_cursor_x = max_x;
    if (g_cursor_y > max_y) g_cursor_y = max_y;
}

bool handle_keyboard(uint32_t keyboard) {
    descriptor_defs::KeyboardEvent events[16]{};
    long bytes = descriptor_read(keyboard, events, sizeof(events));
    if (bytes <= 0) {
        return true;
    }
    size_t count = static_cast<size_t>(bytes) / sizeof(events[0]);
    for (size_t i = 0; i < count; ++i) {
        if ((events[i].flags & descriptor_defs::kKeyboardFlagPressed) != 0 &&
            events[i].scancode == 0x01) {
            return false;
        }
    }
    return true;
}

void handle_mouse(uint32_t mouse, uint8_t& previous_buttons) {
    descriptor_defs::MouseEvent events[24]{};
    long bytes = descriptor_read(mouse, events, sizeof(events));
    if (bytes <= 0) {
        return;
    }
    restore_cursor();
    size_t count = static_cast<size_t>(bytes) / sizeof(events[0]);
    for (size_t i = 0; i < count; ++i) {
        g_cursor_x += events[i].dx;
        g_cursor_y -= events[i].dy;
        clamp_cursor();
        bool left_pressed = (events[i].buttons & 1u) != 0 &&
                            (previous_buttons & 1u) == 0;
        previous_buttons = events[i].buttons;
        int32_t taskbar_y = static_cast<int32_t>(g_fb.height) - 42;
        if (left_pressed && g_cursor_x >= 8 && g_cursor_x < 100 &&
            g_cursor_y >= taskbar_y + 7 && g_cursor_y < taskbar_y + 35) {
            g_menu_open = !g_menu_open;
            draw_desktop();
        }
    }
    draw_cursor();
}

}  // namespace

int main(uint64_t, uint64_t) {
    long session = graphical_session_open();
    if (session < 0) {
        return 1;
    }

    descriptor_defs::GraphicalSessionInfo session_info{};
    if (graphical_session_get_info(static_cast<uint32_t>(session),
                                   &session_info) != 0 ||
        session_info.abi_major != descriptor_defs::kGraphicalSessionAbiMajor) {
        descriptor_close(static_cast<uint32_t>(session));
        return 1;
    }

    long framebuffer = framebuffer_open_slot(session_info.display_slot);
    if (framebuffer < 0) {
        descriptor_close(static_cast<uint32_t>(session));
        return 1;
    }
    g_framebuffer = static_cast<uint32_t>(framebuffer);
    if (framebuffer_get_info(g_framebuffer, &g_fb) != 0 ||
        g_fb.virtual_base == 0 || g_fb.width < 320 || g_fb.height < 200 ||
        g_fb.bpp < 16 || g_fb.bpp > 32 || (g_fb.bpp % 8) != 0) {
        descriptor_close(g_framebuffer);
        descriptor_close(static_cast<uint32_t>(session));
        return 1;
    }
    g_pixels = reinterpret_cast<uint8_t*>(g_fb.virtual_base);
    g_bytes_per_pixel = g_fb.bpp / 8u;

    long keyboard = descriptor_open(kKeyboardType, 0);
    long mouse = mouse_open();
    if (keyboard < 0 || mouse < 0 ||
        graphical_session_set_active(static_cast<uint32_t>(session), true) != 0) {
        if (mouse >= 0) descriptor_close(static_cast<uint32_t>(mouse));
        if (keyboard >= 0) descriptor_close(static_cast<uint32_t>(keyboard));
        descriptor_close(g_framebuffer);
        descriptor_close(static_cast<uint32_t>(session));
        return 1;
    }

    g_cursor_x = static_cast<int32_t>(g_fb.width) / 2;
    g_cursor_y = static_cast<int32_t>(g_fb.height) / 2;
    draw_desktop();
    draw_cursor();

    bool running = true;
    uint8_t previous_buttons = 0;
    while (running) {
        descriptor_defs::DescriptorWait waits[2]{
            {static_cast<uint32_t>(keyboard), descriptor_defs::kWaitRead, 0, 0},
            {static_cast<uint32_t>(mouse), descriptor_defs::kWaitRead, 0, 0},
        };
        if (descriptor_wait(waits, 2) < 0) {
            yield();
            continue;
        }
        if ((waits[0].revents & descriptor_defs::kWaitRead) != 0) {
            running = handle_keyboard(static_cast<uint32_t>(keyboard));
        }
        if (running &&
            (waits[1].revents & descriptor_defs::kWaitRead) != 0) {
            handle_mouse(static_cast<uint32_t>(mouse), previous_buttons);
        }
    }

    restore_cursor();
    (void)graphical_session_set_active(static_cast<uint32_t>(session), false);
    descriptor_close(static_cast<uint32_t>(mouse));
    descriptor_close(static_cast<uint32_t>(keyboard));
    descriptor_close(g_framebuffer);
    descriptor_close(static_cast<uint32_t>(session));
    return 0;
}
