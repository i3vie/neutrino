#include <stddef.h>
#include <stdint.h>

#include "descriptors.hpp"
#include "wm_protocol.hpp"
#include "lattice/lattice.hpp"
#include "../crt/syscall.hpp"

namespace {

constexpr uint32_t kSlot = 1;
constexpr uint32_t kCursorSize = 9;
constexpr uint32_t kTitleBarHeight = 15;
constexpr uint32_t kBorderThickness = 1;
constexpr int32_t kMouseScale = 1;
constexpr size_t kMaxWindows = 8;
constexpr size_t kMaxWindowBytes = 4 * 1024 * 1024;
constexpr const char kRegistryName[] = "wm.registry";
constexpr const char kWindowNamePrefix[] = "wm.win.";

void render_background(uint8_t* frame,
                       const descriptor_defs::FramebufferInfo& info,
                       uint32_t bytes_per_pixel) {
    if (info.width == 0 || info.height == 0) {
        return;
    }
    for (uint32_t y = 0; y < info.height; ++y) {
        for (uint32_t x = 0; x < info.width; ++x) {
            uint32_t r = (x * 255u) / info.width;
            uint32_t g = (y * 255u) / info.height;
            uint32_t b = ((x ^ y) & 0xFFu);
            uint32_t pixel = lattice::pack_color(info, r, g, b);
            lattice::write_pixel(frame, info, bytes_per_pixel, x, y, pixel);
        }
    }
}

struct Window {
    bool in_use;
    bool is_background;
    uint32_t id;
    uint32_t width;
    uint32_t height;
    uint32_t content_height;
    uint32_t stride;
    int32_t x;
    int32_t y;
    uint32_t shm_handle;
    uint8_t* buffer;
    uint32_t in_pipe_handle;
    uint32_t in_pipe_id;
    uint32_t out_pipe_handle;
    uint32_t out_pipe_id;
    char shm_name[48];
    char title[32];
};

void fill_rect_clipped(uint8_t* frame,
                       const descriptor_defs::FramebufferInfo& info,
                       uint32_t bytes_per_pixel,
                       int32_t x,
                       int32_t y,
                       uint32_t width,
                       uint32_t height,
                       uint32_t color,
                       const descriptor_defs::FramebufferRect& clip) {
    int32_t left = x;
    int32_t top = y;
    int32_t right = x + static_cast<int32_t>(width);
    int32_t bottom = y + static_cast<int32_t>(height);
    int32_t clip_left = static_cast<int32_t>(clip.x);
    int32_t clip_top = static_cast<int32_t>(clip.y);
    int32_t clip_right = clip_left + static_cast<int32_t>(clip.width);
    int32_t clip_bottom = clip_top + static_cast<int32_t>(clip.height);
    if (right <= clip_left || bottom <= clip_top ||
        left >= clip_right || top >= clip_bottom) {
        return;
    }
    if (left < clip_left) left = clip_left;
    if (top < clip_top) top = clip_top;
    if (right > clip_right) right = clip_right;
    if (bottom > clip_bottom) bottom = clip_bottom;
    if (right <= left || bottom <= top) {
        return;
    }
    lattice::fill_rect(frame,
              info,
              bytes_per_pixel,
              left,
              top,
              static_cast<uint32_t>(right - left),
              static_cast<uint32_t>(bottom - top),
              color);
}

bool window_rect(const Window& window,
                 const descriptor_defs::FramebufferInfo& info,
                 descriptor_defs::FramebufferRect& out) {
    if (!window.in_use || window.width == 0 || window.height == 0) {
        return false;
    }
    int32_t left = window.x;
    int32_t top = window.y;
    int32_t right = left + static_cast<int32_t>(window.width);
    int32_t bottom = top + static_cast<int32_t>(window.height);
    if (right <= 0 || bottom <= 0) {
        return false;
    }
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > static_cast<int32_t>(info.width)) {
        right = static_cast<int32_t>(info.width);
    }
    if (bottom > static_cast<int32_t>(info.height)) {
        bottom = static_cast<int32_t>(info.height);
    }
    if (right <= left || bottom <= top) {
        return false;
    }
    out.x = static_cast<uint32_t>(left);
    out.y = static_cast<uint32_t>(top);
    out.width = static_cast<uint32_t>(right - left);
    out.height = static_cast<uint32_t>(bottom - top);
    return out.width > 0 && out.height > 0;
}

bool cursor_rect(const descriptor_defs::FramebufferInfo& info,
                 int32_t cursor_x,
                 int32_t cursor_y,
                 descriptor_defs::FramebufferRect& out) {
    int32_t half = static_cast<int32_t>(kCursorSize / 2);
    int32_t left = cursor_x - half;
    int32_t top = cursor_y - half;
    int32_t right = cursor_x + half + 1;
    int32_t bottom = cursor_y + half + 1;
    if (right <= 0 || bottom <= 0) {
        return false;
    }
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > static_cast<int32_t>(info.width)) {
        right = static_cast<int32_t>(info.width);
    }
    if (bottom > static_cast<int32_t>(info.height)) {
        bottom = static_cast<int32_t>(info.height);
    }
    if (right <= left || bottom <= top) {
        return false;
    }
    out.x = static_cast<uint32_t>(left);
    out.y = static_cast<uint32_t>(top);
    out.width = static_cast<uint32_t>(right - left);
    out.height = static_cast<uint32_t>(bottom - top);
    return out.width > 0 && out.height > 0;
}

void union_rect(descriptor_defs::FramebufferRect& base,
                const descriptor_defs::FramebufferRect& add) {
    uint32_t left = (base.x < add.x) ? base.x : add.x;
    uint32_t top = (base.y < add.y) ? base.y : add.y;
    uint32_t right_base = base.x + base.width;
    uint32_t right_add = add.x + add.width;
    uint32_t right = (right_base > right_add) ? right_base : right_add;
    uint32_t bottom_base = base.y + base.height;
    uint32_t bottom_add = add.y + add.height;
    uint32_t bottom = (bottom_base > bottom_add) ? bottom_base : bottom_add;
    base.x = left;
    base.y = top;
    base.width = right - left;
    base.height = bottom - top;
}

bool rect_intersects(const descriptor_defs::FramebufferRect& a,
                     const descriptor_defs::FramebufferRect& b) {
    return !(a.x + a.width <= b.x || b.x + b.width <= a.x ||
             a.y + a.height <= b.y || b.y + b.height <= a.y);
}

void copy_rect(uint8_t* dest,
               const uint8_t* src,
               const descriptor_defs::FramebufferInfo& info,
               uint32_t bytes_per_pixel,
               const descriptor_defs::FramebufferRect& rect) {
    if (dest == nullptr || src == nullptr ||
        rect.width == 0 || rect.height == 0) {
        return;
    }
    size_t row_bytes = static_cast<size_t>(rect.width) * bytes_per_pixel;
    for (uint32_t row = 0; row < rect.height; ++row) {
        size_t offset =
            static_cast<size_t>(rect.y + row) * info.pitch +
            static_cast<size_t>(rect.x) * bytes_per_pixel;
        lattice::copy_bytes(dest + offset, src + offset, row_bytes);
    }
}

void draw_window_decor(uint8_t* frame,
                       const descriptor_defs::FramebufferInfo& info,
                       uint32_t bytes_per_pixel,
                       const Window& window,
                       uint32_t border_color,
                       uint32_t title_color,
                       uint32_t close_fill,
                       uint32_t close_border) {
    (void)close_border;
    if (!window.in_use || window.is_background ||
        window.width == 0 || window.height == 0) {
        return;
    }
    uint32_t title_height = 0;
    if (window.height > window.content_height) {
        title_height = window.height - window.content_height;
    }
    lattice::fill_rect(frame,
              info,
              bytes_per_pixel,
              window.x,
              window.y,
              window.width,
              title_height,
              title_color);
    lattice::fill_rect(frame,
              info,
              bytes_per_pixel,
              window.x,
              window.y,
              window.width,
              kBorderThickness,
              border_color);
    lattice::fill_rect(frame,
              info,
              bytes_per_pixel,
              window.x,
              window.y + static_cast<int32_t>(window.height - kBorderThickness),
              window.width,
              kBorderThickness,
              border_color);
    lattice::fill_rect(frame,
              info,
              bytes_per_pixel,
              window.x,
              window.y,
              kBorderThickness,
              window.height,
              border_color);
    lattice::fill_rect(frame,
              info,
              bytes_per_pixel,
              window.x + static_cast<int32_t>(window.width - kBorderThickness),
              window.y,
              kBorderThickness,
              window.height,
              border_color);

    if (title_height > 0) {
        uint32_t size = title_height;
        if (size > window.width) {
            size = window.width;
        }
        if (size > 0) {
            int32_t left =
                window.x + static_cast<int32_t>(window.width) -
                static_cast<int32_t>(size);
            int32_t top = window.y;
            uint32_t inner_width = size;
            uint32_t inner_height = size;
            if (inner_height > kBorderThickness) {
                top += static_cast<int32_t>(kBorderThickness);
                inner_height -= kBorderThickness;
            }
            if (inner_width > kBorderThickness) {
                inner_width -= kBorderThickness;
            }
            if (inner_width > 0 && inner_height > 0) {
                lattice::fill_rect(frame,
                          info,
                          bytes_per_pixel,
                          left,
                          top,
                          inner_width,
                          inner_height,
                          close_fill);
            }
        }
    }
}

void draw_window_decor_clipped(uint8_t* frame,
                               const descriptor_defs::FramebufferInfo& info,
                               uint32_t bytes_per_pixel,
                               const Window& window,
                               uint32_t border_color,
                               uint32_t title_color,
                               uint32_t close_fill,
                               const descriptor_defs::FramebufferRect& clip) {
    if (!window.in_use || window.is_background ||
        window.width == 0 || window.height == 0) {
        return;
    }
    uint32_t title_height = 0;
    if (window.height > window.content_height) {
        title_height = window.height - window.content_height;
    }
    fill_rect_clipped(frame,
                      info,
                      bytes_per_pixel,
                      window.x,
                      window.y,
                      window.width,
                      title_height,
                      title_color,
                      clip);
    fill_rect_clipped(frame,
                      info,
                      bytes_per_pixel,
                      window.x,
                      window.y,
                      window.width,
                      kBorderThickness,
                      border_color,
                      clip);
    fill_rect_clipped(frame,
                      info,
                      bytes_per_pixel,
                      window.x,
                      window.y + static_cast<int32_t>(window.height - kBorderThickness),
                      window.width,
                      kBorderThickness,
                      border_color,
                      clip);
    fill_rect_clipped(frame,
                      info,
                      bytes_per_pixel,
                      window.x,
                      window.y,
                      kBorderThickness,
                      window.height,
                      border_color,
                      clip);
    fill_rect_clipped(frame,
                      info,
                      bytes_per_pixel,
                      window.x + static_cast<int32_t>(window.width - kBorderThickness),
                      window.y,
                      kBorderThickness,
                      window.height,
                      border_color,
                      clip);

    if (title_height > 0) {
        uint32_t size = title_height;
        if (size > window.width) {
            size = window.width;
        }
        if (size > 0) {
            int32_t left =
                window.x + static_cast<int32_t>(window.width) -
                static_cast<int32_t>(size);
            int32_t top = window.y;
            uint32_t inner_width = size;
            uint32_t inner_height = size;
            if (inner_height > kBorderThickness) {
                top += static_cast<int32_t>(kBorderThickness);
                inner_height -= kBorderThickness;
            }
            if (inner_width > kBorderThickness) {
                inner_width -= kBorderThickness;
            }
            if (inner_width > 0 && inner_height > 0) {
                fill_rect_clipped(frame,
                                  info,
                                  bytes_per_pixel,
                                  left,
                                  top,
                                  inner_width,
                                  inner_height,
                                  close_fill,
                                  clip);
            }
        }
    }
}

bool point_in_window(const Window& window, int32_t x, int32_t y) {
    if (!window.in_use) {
        return false;
    }
    if (x < window.x || y < window.y) {
        return false;
    }
    int32_t right = window.x + static_cast<int32_t>(window.width);
    int32_t bottom = window.y + static_cast<int32_t>(window.height);
    return x < right && y < bottom;
}

bool point_in_titlebar(const Window& window, int32_t x, int32_t y) {
    if (window.is_background || !point_in_window(window, x, y)) {
        return false;
    }
    uint32_t title_height = 0;
    if (window.height > window.content_height) {
        title_height = window.height - window.content_height;
    }
    if (title_height == 0) {
        return false;
    }
    int32_t bottom = window.y + static_cast<int32_t>(title_height);
    return y < bottom;
}

bool close_button_rect(const Window& window,
                       int32_t& left,
                       int32_t& top,
                       uint32_t& size) {
    size = 0;
    if (!window.in_use || window.is_background) {
        return false;
    }
    uint32_t title_height = 0;
    if (window.height > window.content_height) {
        title_height = window.height - window.content_height;
    }
    if (title_height == 0) {
        return false;
    }
    size = title_height;
    if (size > window.width) {
        size = window.width;
    }
    if (size == 0) {
        return false;
    }
    left = window.x + static_cast<int32_t>(window.width) -
           static_cast<int32_t>(size);
    top = window.y;
    if (left < window.x) {
        return false;
    }
    return true;
}

bool point_in_close_button(const Window& window, int32_t x, int32_t y) {
    if (!point_in_titlebar(window, x, y)) {
        return false;
    }
    int32_t left = 0;
    int32_t top = 0;
    uint32_t size = 0;
    if (!close_button_rect(window, left, top, size)) {
        return false;
    }
    int32_t right = left + static_cast<int32_t>(size);
    int32_t bottom = top + static_cast<int32_t>(size);
    return x >= left && x < right && y >= top && y < bottom;
}

int find_window_at(Window (&windows)[kMaxWindows], int32_t x, int32_t y) {
    for (int i = static_cast<int>(kMaxWindows) - 1; i >= 0; --i) {
        if (windows[i].in_use && !windows[i].is_background &&
            point_in_window(windows[i], x, y)) {
            return i;
        }
    }
    return -1;
}

int last_window_index(Window (&windows)[kMaxWindows]) {
    for (int i = static_cast<int>(kMaxWindows) - 1; i >= 0; --i) {
        if (windows[i].in_use && !windows[i].is_background) {
            return i;
        }
    }
    return -1;
}

int window_index(Window (&windows)[kMaxWindows], Window* window) {
    if (window == nullptr) {
        return -1;
    }
    for (size_t i = 0; i < kMaxWindows; ++i) {
        if (&windows[i] == window) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int bring_to_front(Window (&windows)[kMaxWindows],
                   int index,
                   int& focus_index) {
    if (index >= 0 && windows[index].is_background) {
        return index;
    }
    int last = last_window_index(windows);
    if (index < 0 || last < 0 || index == last) {
        return index;
    }
    Window temp = windows[index];
    windows[index] = windows[last];
    windows[last] = temp;
    if (focus_index == index) {
        focus_index = last;
    } else if (focus_index == last) {
        focus_index = index;
    }
    return last;
}

void clamp_window_position(Window& window,
                           const descriptor_defs::FramebufferInfo& info) {
    int32_t max_x = static_cast<int32_t>(info.width) -
                    static_cast<int32_t>(window.width);
    int32_t max_y = static_cast<int32_t>(info.height) -
                    static_cast<int32_t>(window.height);
    if (max_x < 0) {
        max_x = 0;
    }
    if (max_y < 0) {
        max_y = 0;
    }
    if (window.x < 0) {
        window.x = 0;
    }
    if (window.y < 0) {
        window.y = 0;
    }
    if (window.x > max_x) {
        window.x = max_x;
    }
    if (window.y > max_y) {
        window.y = max_y;
    }
}

bool build_window_name(uint32_t id, char* out, size_t out_size) {
    if (out == nullptr || out_size == 0) {
        return false;
    }
    size_t prefix_len = 0;
    while (kWindowNamePrefix[prefix_len] != '\0') {
        ++prefix_len;
    }
    if (prefix_len + 8 + 1 > out_size) {
        return false;
    }
    for (size_t i = 0; i < prefix_len; ++i) {
        out[i] = kWindowNamePrefix[i];
    }
    for (size_t i = 0; i < 8; ++i) {
        uint8_t nibble =
            static_cast<uint8_t>((id >> ((7 - i) * 4)) & 0xFu);
        char ch = (nibble < 10) ? static_cast<char>('0' + nibble)
                                : static_cast<char>('a' + (nibble - 10));
        out[prefix_len + i] = ch;
    }
    out[prefix_len + 8] = '\0';
    return true;
}

void blit_window(uint8_t* frame,
                 const descriptor_defs::FramebufferInfo& info,
                 uint32_t bytes_per_pixel,
                 const Window& window) {
    if (!window.in_use || window.buffer == nullptr || frame == nullptr) {
        return;
    }
    if (window.width == 0 || window.content_height == 0) {
        return;
    }
    uint32_t title_height = 0;
    if (window.height > window.content_height) {
        title_height = window.height - window.content_height;
    }
    int32_t dest_y = window.y + static_cast<int32_t>(title_height);
    if (window.x >= static_cast<int32_t>(info.width) ||
        dest_y >= static_cast<int32_t>(info.height)) {
        return;
    }
    uint32_t copy_width = window.width;
    uint32_t copy_height = window.content_height;
    if (window.x + static_cast<int32_t>(copy_width) >
        static_cast<int32_t>(info.width)) {
        copy_width = info.width - static_cast<uint32_t>(window.x);
    }
    if (dest_y + static_cast<int32_t>(copy_height) >
        static_cast<int32_t>(info.height)) {
        copy_height = info.height - static_cast<uint32_t>(dest_y);
    }
    size_t row_bytes = static_cast<size_t>(copy_width) * bytes_per_pixel;
    for (uint32_t row = 0; row < copy_height; ++row) {
        size_t dest_offset =
            static_cast<size_t>(dest_y + static_cast<int32_t>(row)) *
                info.pitch +
            static_cast<size_t>(window.x) * bytes_per_pixel;
        size_t src_offset =
            static_cast<size_t>(row) * window.stride;
        lattice::copy_bytes(frame + dest_offset,
                            window.buffer + src_offset,
                            row_bytes);
    }
}

void blit_window_clipped(uint8_t* frame,
                         const descriptor_defs::FramebufferInfo& info,
                         uint32_t bytes_per_pixel,
                         const Window& window,
                         const descriptor_defs::FramebufferRect& clip) {
    if (!window.in_use || window.buffer == nullptr || frame == nullptr) {
        return;
    }
    if (window.width == 0 || window.content_height == 0) {
        return;
    }
    uint32_t title_height = 0;
    if (window.height > window.content_height) {
        title_height = window.height - window.content_height;
    }
    int32_t content_left = window.x;
    int32_t content_top = window.y + static_cast<int32_t>(title_height);
    int32_t content_right = content_left + static_cast<int32_t>(window.width);
    int32_t content_bottom =
        content_top + static_cast<int32_t>(window.content_height);

    int32_t clip_left = static_cast<int32_t>(clip.x);
    int32_t clip_top = static_cast<int32_t>(clip.y);
    int32_t clip_right = clip_left + static_cast<int32_t>(clip.width);
    int32_t clip_bottom = clip_top + static_cast<int32_t>(clip.height);

    if (content_right <= clip_left || content_bottom <= clip_top ||
        content_left >= clip_right || content_top >= clip_bottom) {
        return;
    }

    int32_t left = content_left > clip_left ? content_left : clip_left;
    int32_t top = content_top > clip_top ? content_top : clip_top;
    int32_t right = content_right < clip_right ? content_right : clip_right;
    int32_t bottom = content_bottom < clip_bottom ? content_bottom : clip_bottom;
    if (right <= left || bottom <= top) {
        return;
    }

    uint32_t copy_width = static_cast<uint32_t>(right - left);
    uint32_t copy_height = static_cast<uint32_t>(bottom - top);
    size_t row_bytes = static_cast<size_t>(copy_width) * bytes_per_pixel;
    size_t src_x = static_cast<size_t>(left - content_left);
    size_t src_y = static_cast<size_t>(top - content_top);

    for (uint32_t row = 0; row < copy_height; ++row) {
        size_t dest_offset =
            static_cast<size_t>(top + static_cast<int32_t>(row)) * info.pitch +
            static_cast<size_t>(left) * bytes_per_pixel;
        size_t src_offset =
            (src_y + row) * window.stride + src_x * bytes_per_pixel;
        lattice::copy_bytes(frame + dest_offset,
                            window.buffer + src_offset,
                            row_bytes);
    }
}

void draw_cursor(uint8_t* frame,
                 const descriptor_defs::FramebufferInfo& info,
                 uint32_t bytes_per_pixel,
                 int32_t cursor_x,
                 int32_t cursor_y,
                 uint32_t color) {
    if (frame == nullptr) {
        return;
    }
    int32_t half = static_cast<int32_t>(kCursorSize / 2);
    for (int32_t dx = -half; dx <= half; ++dx) {
        int32_t x = cursor_x + dx;
        if (x < 0 || x >= static_cast<int32_t>(info.width)) {
            continue;
        }
        if (cursor_y < 0 || cursor_y >= static_cast<int32_t>(info.height)) {
            continue;
        }
        lattice::write_pixel(frame,
                             info,
                             bytes_per_pixel,
                             static_cast<uint32_t>(x),
                             static_cast<uint32_t>(cursor_y),
                             color);
    }
    for (int32_t dy = -half; dy <= half; ++dy) {
        int32_t y = cursor_y + dy;
        if (y < 0 || y >= static_cast<int32_t>(info.height)) {
            continue;
        }
        if (cursor_x < 0 || cursor_x >= static_cast<int32_t>(info.width)) {
            continue;
        }
        lattice::write_pixel(frame,
                             info,
                             bytes_per_pixel,
                             static_cast<uint32_t>(cursor_x),
                             static_cast<uint32_t>(y),
                             color);
    }
}

void send_key(Window* window, uint8_t key) {
    if (window == nullptr || !window->in_use || window->in_pipe_handle == 0) {
        return;
    }
    descriptor_write(window->in_pipe_handle, &key, 1);
}

bool drain_present(Window& window) {
    if (!window.in_use || window.out_pipe_handle == 0) {
        return false;
    }
    uint8_t buffer[16];
    bool changed = false;
    while (true) {
        long read = descriptor_read(window.out_pipe_handle,
                                    buffer,
                                    sizeof(buffer));
        if (read <= 0) {
            break;
        }
        for (long i = 0; i < read; ++i) {
            if (buffer[i] ==
                static_cast<uint8_t>(wm::ClientMessage::Present)) {
                changed = true;
            }
        }
    }
    return changed;
}

void reset_window(Window& window) {
    window.in_use = false;
    window.is_background = false;
    window.id = 0;
    window.width = 0;
    window.height = 0;
    window.content_height = 0;
    window.stride = 0;
    window.x = 0;
    window.y = 0;
    window.shm_handle = 0;
    window.buffer = nullptr;
    window.in_pipe_handle = 0;
    window.in_pipe_id = 0;
    window.out_pipe_handle = 0;
    window.out_pipe_id = 0;
    window.shm_name[0] = '\0';
    window.title[0] = '\0';
}

void close_window(Window& window) {
    if (!window.in_use) {
        return;
    }
    if (window.in_pipe_handle != 0) {
        uint8_t msg = static_cast<uint8_t>(wm::ServerMessage::Close);
        descriptor_write(window.in_pipe_handle, &msg, 1);
    }
    if (window.buffer != nullptr &&
        window.stride != 0 &&
        window.content_height != 0) {
        size_t bytes =
            static_cast<size_t>(window.stride) * window.content_height;
        unmap(window.buffer, bytes);
    }
    if (window.in_pipe_handle != 0) {
        descriptor_close(window.in_pipe_handle);
    }
    if (window.out_pipe_handle != 0) {
        descriptor_close(window.out_pipe_handle);
    }
    if (window.shm_handle != 0) {
        descriptor_close(window.shm_handle);
    }
    reset_window(window);
}

Window* allocate_window(Window (&windows)[kMaxWindows]) {
    for (size_t i = 0; i < kMaxWindows; ++i) {
        if (!windows[i].in_use) {
            reset_window(windows[i]);
            windows[i].in_use = true;
            return &windows[i];
        }
    }
    return nullptr;
}

bool write_pipe_all(uint32_t handle, const void* data, size_t length) {
    if (data == nullptr || length == 0) {
        return false;
    }
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(data);
    size_t offset = 0;
    while (offset < length) {
        long written = descriptor_write(handle, bytes + offset, length - offset);
        if (written <= 0) {
            return false;
        }
        offset += static_cast<size_t>(written);
    }
    return true;
}

bool populate_registry(uint32_t server_pipe_id) {
    long handle = shared_memory_open(kRegistryName, sizeof(wm::Registry));
    if (handle < 0) {
        return false;
    }
    descriptor_defs::SharedMemoryInfo info{};
    if (shared_memory_get_info(static_cast<uint32_t>(handle), &info) != 0 ||
        info.base == 0 || info.length < sizeof(wm::Registry)) {
        descriptor_close(static_cast<uint32_t>(handle));
        return false;
    }
    wm::Registry* registry = reinterpret_cast<wm::Registry*>(info.base);
    registry->magic = wm::kRegistryMagic;
    registry->version = wm::kRegistryVersion;
    registry->reserved = 0;
    registry->server_pipe_id = server_pipe_id;
    return true;
}

bool create_window_from_request(const wm::CreateRequest& request,
                                const descriptor_defs::FramebufferInfo& info,
                                uint32_t bytes_per_pixel,
                                Window (&windows)[kMaxWindows],
                                uint32_t& next_window_id,
                                int& background_index,
                                Window*& out_window,
                                wm::CreateResponse& response) {
    response = {};
    response.type = static_cast<uint32_t>(wm::MessageType::CreateWindow);
    response.status = -1;
    out_window = nullptr;

    if (request.reply_pipe_id == 0) {
        return false;
    }

    bool request_background =
        (request.flags & wm::kWindowFlagBackground) != 0;
    if (request_background && background_index >= 0) {
        response.status = -10;
        return false;
    }

    Window* window = allocate_window(windows);
    if (window == nullptr) {
        response.status = -2;
        return false;
    }
    window->is_background = request_background;

    uint32_t width = request.width;
    uint32_t height = request.height;
    if (request_background) {
        width = info.width;
        height = info.height;
    } else {
        if (width == 0 || height == 0) {
            width = info.width;
            height = info.height;
        }
        if (width > info.width) {
            width = info.width;
        }
        if (height > info.height) {
            height = info.height;
        }
    }

    uint32_t title_height = 0;
    if (!request_background && info.height > kTitleBarHeight) {
        title_height = kTitleBarHeight;
    }
    if (title_height > 0 && height + title_height > info.height) {
        if (info.height > title_height) {
            height = info.height - title_height;
        } else {
            height = 0;
        }
    }

    uint64_t stride = static_cast<uint64_t>(width) * bytes_per_pixel;
    uint64_t total = stride * height;
    if (stride == 0 || total == 0 || total > kMaxWindowBytes) {
        window->in_use = false;
        response.status = -3;
        return false;
    }

    uint32_t window_id = next_window_id++;
    char shm_name[sizeof(window->shm_name)]{};
    if (!build_window_name(window_id, shm_name, sizeof(shm_name))) {
        window->in_use = false;
        response.status = -4;
        return false;
    }

    long shm_handle = shared_memory_open(shm_name, static_cast<size_t>(total));
    if (shm_handle < 0) {
        window->in_use = false;
        response.status = -5;
        return false;
    }

    descriptor_defs::SharedMemoryInfo shm_info{};
    if (shared_memory_get_info(static_cast<uint32_t>(shm_handle), &shm_info) != 0 ||
        shm_info.base == 0 || shm_info.length < total) {
        descriptor_close(static_cast<uint32_t>(shm_handle));
        window->in_use = false;
        response.status = -6;
        return false;
    }

    uint64_t in_flags = static_cast<uint64_t>(descriptor_defs::Flag::Readable) |
                        static_cast<uint64_t>(descriptor_defs::Flag::Async);
    long in_handle = pipe_open_new(in_flags);
    if (in_handle < 0) {
        descriptor_close(static_cast<uint32_t>(shm_handle));
        window->in_use = false;
        response.status = -7;
        return false;
    }
    descriptor_defs::PipeInfo in_info{};
    if (pipe_get_info(static_cast<uint32_t>(in_handle), &in_info) != 0 ||
        in_info.id == 0) {
        descriptor_close(static_cast<uint32_t>(in_handle));
        descriptor_close(static_cast<uint32_t>(shm_handle));
        window->in_use = false;
        response.status = -8;
        return false;
    }

    uint64_t out_flags = static_cast<uint64_t>(descriptor_defs::Flag::Writable) |
                         static_cast<uint64_t>(descriptor_defs::Flag::Async);
    long out_handle = pipe_open_existing(out_flags, request.reply_pipe_id);
    if (out_handle < 0) {
        descriptor_close(static_cast<uint32_t>(in_handle));
        descriptor_close(static_cast<uint32_t>(shm_handle));
        window->in_use = false;
        response.status = -9;
        return false;
    }

    window->id = window_id;
    window->width = width;
    window->content_height = height;
    window->height = height + title_height;
    window->stride = static_cast<uint32_t>(stride);
    if (request_background) {
        window->x = 0;
        window->y = 0;
    } else {
        int32_t base = static_cast<int32_t>((window_id - 1) * 24u);
        window->x = base;
        window->y = base + static_cast<int32_t>(title_height);
        clamp_window_position(*window, info);
    }
    window->shm_handle = static_cast<uint32_t>(shm_handle);
    window->buffer = reinterpret_cast<uint8_t*>(shm_info.base);
    window->in_pipe_handle = static_cast<uint32_t>(out_handle);
    window->in_pipe_id = request.reply_pipe_id;
    window->out_pipe_handle = static_cast<uint32_t>(in_handle);
    window->out_pipe_id = in_info.id;
    for (size_t i = 0; i < sizeof(window->shm_name); ++i) {
        window->shm_name[i] = shm_name[i];
        if (shm_name[i] == '\0') {
            break;
        }
    }
    for (size_t i = 0; i < sizeof(window->title); ++i) {
        window->title[i] = request.title[i];
        if (request.title[i] == '\0') {
            break;
        }
    }
    window->title[sizeof(window->title) - 1] = '\0';

    response.status = 0;
    response.window_id = window->id;
    response.width = window->width;
    response.height = window->content_height;
    response.stride = window->stride;
    response.x = window->x;
    response.y = window->y;
    response.in_pipe_id = window->in_pipe_id;
    response.out_pipe_id = window->out_pipe_id;
    for (size_t i = 0; i < sizeof(response.shm_name); ++i) {
        response.shm_name[i] = window->shm_name[i];
        if (window->shm_name[i] == '\0') {
            break;
        }
    }
    response.format.bpp = info.bpp;
    response.format.red_mask_size = info.red_mask_size;
    response.format.red_mask_shift = info.red_mask_shift;
    response.format.green_mask_size = info.green_mask_size;
    response.format.green_mask_shift = info.green_mask_shift;
    response.format.blue_mask_size = info.blue_mask_size;
    response.format.blue_mask_shift = info.blue_mask_shift;
    if (request_background) {
        int idx = window_index(windows, window);
        if (idx >= 0) {
            background_index = idx;
        }
    }
    out_window = window;
    return true;
}

bool compute_dirty_rect(const descriptor_defs::FramebufferInfo& info,
                        int32_t prev_x,
                        int32_t prev_y,
                        int32_t cursor_x,
                        int32_t cursor_y,
                        descriptor_defs::FramebufferRect& rect) {
    int32_t half = static_cast<int32_t>(kCursorSize / 2);
    int32_t left = prev_x < cursor_x ? prev_x : cursor_x;
    int32_t right = prev_x > cursor_x ? prev_x : cursor_x;
    int32_t top = prev_y < cursor_y ? prev_y : cursor_y;
    int32_t bottom = prev_y > cursor_y ? prev_y : cursor_y;
    left -= half;
    right += half;
    top -= half;
    bottom += half;
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right >= static_cast<int32_t>(info.width)) {
        right = static_cast<int32_t>(info.width) - 1;
    }
    if (bottom >= static_cast<int32_t>(info.height)) {
        bottom = static_cast<int32_t>(info.height) - 1;
    }
    if (left > right || top > bottom) {
        return false;
    }
    rect.x = static_cast<uint32_t>(left);
    rect.y = static_cast<uint32_t>(top);
    rect.width = static_cast<uint32_t>(right - left + 1);
    rect.height = static_cast<uint32_t>(bottom - top + 1);
    return rect.width > 0 && rect.height > 0;
}

void render_cursor_region_mapped(uint8_t* dest,
                                 const uint8_t* background,
                                 const descriptor_defs::FramebufferInfo& info,
                                 uint32_t bytes_per_pixel,
                                 const descriptor_defs::FramebufferRect& rect,
                                 int32_t cursor_x,
                                 int32_t cursor_y,
                                 uint32_t color) {
    if (dest == nullptr || background == nullptr) {
        return;
    }
    if (rect.width == 0 || rect.height == 0) {
        return;
    }
    int32_t half = static_cast<int32_t>(kCursorSize / 2);
    for (uint32_t row = 0; row < rect.height; ++row) {
        uint32_t y = rect.y + row;
        size_t base_offset = static_cast<size_t>(y) * info.pitch +
                             static_cast<size_t>(rect.x) * bytes_per_pixel;
        size_t row_bytes = static_cast<size_t>(rect.width) * bytes_per_pixel;
        for (size_t i = 0; i < row_bytes; ++i) {
            dest[base_offset + i] = background[base_offset + i];
        }

        if (static_cast<int32_t>(y) == cursor_y) {
            int32_t h_start = cursor_x - half;
            int32_t h_end = cursor_x + half;
            int32_t rect_right =
                static_cast<int32_t>(rect.x + rect.width - 1);
            if (h_start < static_cast<int32_t>(rect.x)) {
                h_start = static_cast<int32_t>(rect.x);
            }
            if (h_end > rect_right) {
                h_end = rect_right;
            }
            for (int32_t x = h_start; x <= h_end; ++x) {
                size_t offset =
                    base_offset +
                    static_cast<size_t>(x - static_cast<int32_t>(rect.x)) *
                        bytes_per_pixel;
                lattice::write_pixel_raw(dest, bytes_per_pixel, offset, color);
            }
        }

        int32_t v_offset = static_cast<int32_t>(y) - cursor_y;
        if (v_offset >= -half && v_offset <= half) {
            if (cursor_x >= static_cast<int32_t>(rect.x) &&
                cursor_x <= static_cast<int32_t>(rect.x + rect.width - 1)) {
                size_t offset =
                    base_offset +
                    static_cast<size_t>(cursor_x -
                                        static_cast<int32_t>(rect.x)) *
                        bytes_per_pixel;
                lattice::write_pixel_raw(dest, bytes_per_pixel, offset, color);
            }
        }
    }
}

void render_cursor_region(uint32_t handle,
                          const uint8_t* background,
                          const descriptor_defs::FramebufferInfo& info,
                          uint32_t bytes_per_pixel,
                          int32_t prev_x,
                          int32_t prev_y,
                          int32_t cursor_x,
                          int32_t cursor_y,
                          uint32_t color,
                          uint8_t* row_buffer,
                          size_t row_buffer_bytes) {
    int32_t half = static_cast<int32_t>(kCursorSize / 2);
    int32_t left = prev_x < cursor_x ? prev_x : cursor_x;
    int32_t right = prev_x > cursor_x ? prev_x : cursor_x;
    int32_t top = prev_y < cursor_y ? prev_y : cursor_y;
    int32_t bottom = prev_y > cursor_y ? prev_y : cursor_y;
    left -= half;
    right += half;
    top -= half;
    bottom += half;
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right >= static_cast<int32_t>(info.width)) {
        right = static_cast<int32_t>(info.width) - 1;
    }
    if (bottom >= static_cast<int32_t>(info.height)) {
        bottom = static_cast<int32_t>(info.height) - 1;
    }
    if (left > right || top > bottom) {
        return;
    }

    uint32_t rect_width = static_cast<uint32_t>(right - left + 1);
    size_t row_bytes = static_cast<size_t>(rect_width) * bytes_per_pixel;
    if (row_bytes > row_buffer_bytes) {
        return;
    }

    for (int32_t y = top; y <= bottom; ++y) {
        size_t src_offset = static_cast<size_t>(y) * info.pitch +
                            static_cast<size_t>(left) * bytes_per_pixel;
        for (size_t i = 0; i < row_bytes; ++i) {
            row_buffer[i] = background[src_offset + i];
        }

        if (y == cursor_y) {
            int32_t h_start = cursor_x - half;
            int32_t h_end = cursor_x + half;
            if (h_start < left) h_start = left;
            if (h_end > right) h_end = right;
            for (int32_t x = h_start; x <= h_end; ++x) {
                size_t offset =
                    static_cast<size_t>(x - left) * bytes_per_pixel;
                lattice::write_pixel_raw(row_buffer, bytes_per_pixel, offset, color);
            }
        }

        int32_t v_offset = y - cursor_y;
        if (v_offset >= -half && v_offset <= half) {
            if (cursor_x >= left && cursor_x <= right) {
                size_t offset =
                    static_cast<size_t>(cursor_x - left) * bytes_per_pixel;
                lattice::write_pixel_raw(row_buffer, bytes_per_pixel, offset, color);
            }
        }

        uint64_t dest_offset = static_cast<uint64_t>(y) * info.pitch +
                               static_cast<uint64_t>(left) * bytes_per_pixel;
        descriptor_write(handle, row_buffer, row_bytes, dest_offset);
    }
}

}  // namespace

int main(uint64_t, uint64_t) {
    long fb = framebuffer_open_slot(kSlot);
    if (fb < 0) {
        return 1;
    }

    descriptor_defs::FramebufferInfo info{};
    if (framebuffer_get_info(static_cast<uint32_t>(fb), &info) != 0) {
        descriptor_close(static_cast<uint32_t>(fb));
        return 1;
    }

    uint32_t bytes_per_pixel = (info.bpp + 7u) / 8u;
    if (bytes_per_pixel == 0 || bytes_per_pixel > 4 ||
        info.width == 0 || info.height == 0) {
        descriptor_close(static_cast<uint32_t>(fb));
        return 1;
    }

    size_t frame_bytes = static_cast<size_t>(info.pitch) * info.height;
    uint8_t* frame =
        static_cast<uint8_t*>(map_anonymous(frame_bytes, MAP_WRITE));
    if (frame == nullptr) {
        descriptor_close(static_cast<uint32_t>(fb));
        return 1;
    }

    uint8_t* background =
        static_cast<uint8_t*>(map_anonymous(frame_bytes, MAP_WRITE));
    if (background == nullptr) {
        descriptor_close(static_cast<uint32_t>(fb));
        return 1;
    }

    uint8_t* fb_ptr = reinterpret_cast<uint8_t*>(info.virtual_base);
    bool use_mapping = fb_ptr != nullptr;

    change_slot(kSlot);

    uint64_t server_flags = static_cast<uint64_t>(descriptor_defs::Flag::Readable) |
                            static_cast<uint64_t>(descriptor_defs::Flag::Async);
    long server_pipe = pipe_open_new(server_flags);
    if (server_pipe < 0) {
        descriptor_close(static_cast<uint32_t>(fb));
        return 1;
    }
    descriptor_defs::PipeInfo server_info{};
    if (pipe_get_info(static_cast<uint32_t>(server_pipe), &server_info) != 0 ||
        server_info.id == 0) {
        descriptor_close(static_cast<uint32_t>(server_pipe));
        descriptor_close(static_cast<uint32_t>(fb));
        return 1;
    }
    if (!populate_registry(server_info.id)) {
        descriptor_close(static_cast<uint32_t>(server_pipe));
        descriptor_close(static_cast<uint32_t>(fb));
        return 1;
    }

    long mouse = mouse_open();
    if (mouse < 0) {
        descriptor_close(static_cast<uint32_t>(fb));
        return 1;
    }

    long keyboard = descriptor_open(
        static_cast<uint32_t>(descriptor_defs::Type::Keyboard), 0, 0, 0);
    if (keyboard < 0) {
        descriptor_close(static_cast<uint32_t>(fb));
        descriptor_close(static_cast<uint32_t>(mouse));
        return 1;
    }

    int32_t cursor_x = static_cast<int32_t>(info.width / 2);
    int32_t cursor_y = static_cast<int32_t>(info.height / 2);
    int32_t prev_x = cursor_x;
    int32_t prev_y = cursor_y;
    bool scene_dirty = true;
    bool had_windows = false;
    bool force_full_redraw = true;
    bool dirty_rect_valid = false;
    descriptor_defs::FramebufferRect dirty_rect{};

    Window windows[kMaxWindows]{};
    for (size_t i = 0; i < kMaxWindows; ++i) {
        reset_window(windows[i]);
    }
    uint32_t next_window_id = 1;
    size_t window_count = 0;
    int background_index = -1;
    int focus_index = -1;
    int drag_index = -1;
    int32_t drag_offset_x = 0;
    int32_t drag_offset_y = 0;
    bool left_down = false;
    uint8_t request_buffer[sizeof(wm::CreateRequest)]{};
    size_t request_fill = 0;

    uint32_t border_color = lattice::pack_color(info, 210, 210, 220);
    uint32_t title_color = lattice::pack_color(info, 40, 40, 48);
    uint32_t title_focus_color = lattice::pack_color(info, 70, 90, 140);
    uint32_t close_fill = lattice::pack_color(info, 210, 70, 70);
    uint32_t close_border = lattice::pack_color(info, 120, 30, 30);

    auto mark_dirty_rect = [&](const descriptor_defs::FramebufferRect& rect) {
        if (rect.width == 0 || rect.height == 0) {
            return;
        }
        if (!dirty_rect_valid) {
            dirty_rect = rect;
            dirty_rect_valid = true;
        } else {
            union_rect(dirty_rect, rect);
        }
    };

    render_background(background, info, bytes_per_pixel);
    lattice::copy_bytes(frame, background, frame_bytes);
    if (use_mapping) {
        for (size_t i = 0; i < frame_bytes; ++i) {
            fb_ptr[i] = frame[i];
        }
        framebuffer_present(static_cast<uint32_t>(fb), nullptr);
    } else {
        descriptor_write(static_cast<uint32_t>(fb), frame, frame_bytes, 0);
    }

    size_t row_buffer_bytes = 0;
    uint8_t* row_buffer = nullptr;
    if (!use_mapping) {
        row_buffer_bytes = info.pitch;
        row_buffer =
            static_cast<uint8_t*>(map_anonymous(row_buffer_bytes, MAP_WRITE));
        if (row_buffer == nullptr) {
            descriptor_close(static_cast<uint32_t>(keyboard));
            descriptor_close(static_cast<uint32_t>(mouse));
            descriptor_close(static_cast<uint32_t>(fb));
            return 1;
        }
    }

    while (1) {
        descriptor_defs::MouseEvent events[16];
        long bytes = descriptor_read(static_cast<uint32_t>(mouse),
                                     events,
                                     sizeof(events));
        if (bytes > 0) {
            size_t count =
                static_cast<size_t>(bytes) / sizeof(descriptor_defs::MouseEvent);
            for (size_t i = 0; i < count; ++i) {
                int32_t nx =
                    cursor_x +
                    static_cast<int32_t>(events[i].dx) * kMouseScale;
                int32_t ny =
                    cursor_y -
                    static_cast<int32_t>(events[i].dy) * kMouseScale;
                if (nx < 0) nx = 0;
                if (ny < 0) ny = 0;
                if (nx >= static_cast<int32_t>(info.width)) {
                    nx = static_cast<int32_t>(info.width) - 1;
                }
                if (ny >= static_cast<int32_t>(info.height)) {
                    ny = static_cast<int32_t>(info.height) - 1;
                }
                cursor_x = nx;
                cursor_y = ny;
                bool left = (events[i].buttons & 0x1u) != 0;
                if (left && !left_down) {
                    int prev_focus = focus_index;
                    int hit = find_window_at(windows, cursor_x, cursor_y);
                    if (hit >= 0) {
                        descriptor_defs::FramebufferRect prev_focus_rect{};
                        bool has_prev_focus_rect = false;
                        if (prev_focus >= 0 &&
                            prev_focus < static_cast<int>(kMaxWindows) &&
                            !windows[prev_focus].is_background) {
                            has_prev_focus_rect =
                                window_rect(windows[prev_focus],
                                            info,
                                            prev_focus_rect);
                        }
                        descriptor_defs::FramebufferRect hit_rect{};
                        bool has_hit_rect =
                            window_rect(windows[hit], info, hit_rect);
                        int last = last_window_index(windows);
                        if (last >= 0 && hit != last) {
                            descriptor_defs::FramebufferRect rect{};
                            if (window_rect(windows[hit], info, rect)) {
                                mark_dirty_rect(rect);
                            }
                            if (window_rect(windows[last], info, rect)) {
                                mark_dirty_rect(rect);
                            }
                        }
                        focus_index = hit;
                        hit = bring_to_front(windows, hit, focus_index);
                        scene_dirty = true;
                        if (prev_focus != focus_index) {
                            if (has_prev_focus_rect) {
                                mark_dirty_rect(prev_focus_rect);
                            }
                            if (has_hit_rect) {
                                mark_dirty_rect(hit_rect);
                            }
                        }
                        if (point_in_close_button(windows[hit],
                                                  cursor_x,
                                                  cursor_y)) {
                            descriptor_defs::FramebufferRect rect{};
                            if (window_rect(windows[hit], info, rect)) {
                                mark_dirty_rect(rect);
                            }
                            bool was_background = windows[hit].is_background;
                            close_window(windows[hit]);
                            if (was_background) {
                                background_index = -1;
                            } else if (window_count > 0) {
                                --window_count;
                            }
                            if (focus_index == hit) {
                                focus_index = last_window_index(windows);
                                if (focus_index < 0 &&
                                    background_index >= 0 &&
                                    background_index < static_cast<int>(kMaxWindows) &&
                                    windows[background_index].in_use) {
                                    focus_index = background_index;
                                }
                            }
                            drag_index = -1;
                            force_full_redraw = true;
                            scene_dirty = true;
                        } else if (point_in_titlebar(windows[hit],
                                                     cursor_x,
                                                     cursor_y)) {
                            drag_index = hit;
                            drag_offset_x = cursor_x - windows[hit].x;
                            drag_offset_y = cursor_y - windows[hit].y;
                        }
                    } else if (background_index >= 0 &&
                               background_index < static_cast<int>(kMaxWindows) &&
                               windows[background_index].in_use) {
                        descriptor_defs::FramebufferRect prev_focus_rect{};
                        bool has_prev_focus_rect = false;
                        if (prev_focus >= 0 &&
                            prev_focus < static_cast<int>(kMaxWindows) &&
                            windows[prev_focus].in_use &&
                            !windows[prev_focus].is_background) {
                            has_prev_focus_rect =
                                window_rect(windows[prev_focus],
                                            info,
                                            prev_focus_rect);
                        }
                        focus_index = background_index;
                        if (prev_focus != focus_index && has_prev_focus_rect) {
                            mark_dirty_rect(prev_focus_rect);
                            scene_dirty = true;
                        }
                    }
                }
                if (!left && left_down) {
                    drag_index = -1;
                }
                if (left && drag_index >= 0) {
                    Window& win = windows[drag_index];
                    descriptor_defs::FramebufferRect rect{};
                    bool has_old = window_rect(win, info, rect);
                    int32_t new_x = cursor_x - drag_offset_x;
                    int32_t new_y = cursor_y - drag_offset_y;
                    if (new_x != win.x || new_y != win.y) {
                        win.x = new_x;
                        win.y = new_y;
                        clamp_window_position(win, info);
                        if (has_old) {
                            mark_dirty_rect(rect);
                        }
                        if (window_rect(win, info, rect)) {
                            mark_dirty_rect(rect);
                        }
                        scene_dirty = true;
                    }
                }
                left_down = left;
            }
        }

        uint8_t key = 0;
        long kread = descriptor_read(static_cast<uint32_t>(keyboard),
                                     &key,
                                     1);
        if (kread > 0) {
            bool has_background_for_exit =
                background_index >= 0 &&
                background_index < static_cast<int>(kMaxWindows) &&
                windows[background_index].in_use;
            if (key == 27 && window_count == 0 && !has_background_for_exit) {
                break;
            }
            if (focus_index >= 0 &&
                focus_index < static_cast<int>(kMaxWindows) &&
                windows[focus_index].in_use) {
                send_key(&windows[focus_index], key);
            }
        }

        while (true) {
            long read_bytes = descriptor_read(
                static_cast<uint32_t>(server_pipe),
                request_buffer + request_fill,
                sizeof(request_buffer) - request_fill);
            if (read_bytes <= 0) {
                break;
            }
            request_fill += static_cast<size_t>(read_bytes);
            if (request_fill < sizeof(request_buffer)) {
                continue;
            }
            wm::CreateRequest request{};
            lattice::copy_bytes(reinterpret_cast<uint8_t*>(&request),
                                 request_buffer,
                                 sizeof(request));
            request_fill = 0;

            if (request.type == static_cast<uint32_t>(wm::MessageType::CreateWindow)) {
                wm::CreateResponse response{};
                Window* created = nullptr;
                bool ok = create_window_from_request(request,
                                                     info,
                                                     bytes_per_pixel,
                                                     windows,
                                                     next_window_id,
                                                     background_index,
                                                     created,
                                                     response);
                uint32_t reply_handle = 0;
                bool close_reply = false;
                if (created != nullptr) {
                    reply_handle = created->in_pipe_handle;
                } else {
                    uint64_t reply_flags =
                        static_cast<uint64_t>(descriptor_defs::Flag::Writable);
                    long handle = pipe_open_existing(reply_flags,
                                                     request.reply_pipe_id);
                    if (handle >= 0) {
                        reply_handle = static_cast<uint32_t>(handle);
                        close_reply = true;
                    }
                }
                if (reply_handle != 0) {
                    write_pipe_all(reply_handle, &response, sizeof(response));
                    if (close_reply) {
                        descriptor_close(reply_handle);
                    }
                }
                if (ok) {
                    int created_index = window_index(windows, created);
                    if (created != nullptr && created->is_background) {
                        if (created_index >= 0 &&
                            (focus_index < 0 || window_count == 0)) {
                            focus_index = created_index;
                        }
                    } else {
                        ++window_count;
                        if (created_index >= 0) {
                            focus_index = created_index;
                            focus_index =
                                bring_to_front(windows,
                                               created_index,
                                               focus_index);
                        }
                    }
                    descriptor_defs::FramebufferRect rect{};
                    if (created != nullptr &&
                        window_rect(*created, info, rect)) {
                        mark_dirty_rect(rect);
                    }
                    scene_dirty = true;
                }
            }
        }

        for (size_t i = 0; i < kMaxWindows; ++i) {
            if (windows[i].in_use && drain_present(windows[i])) {
                descriptor_defs::FramebufferRect rect{};
                if (window_rect(windows[i], info, rect)) {
                    mark_dirty_rect(rect);
                }
                scene_dirty = true;
            }
        }

        bool has_background =
            background_index >= 0 &&
            background_index < static_cast<int>(kMaxWindows) &&
            windows[background_index].in_use;
        bool has_windows = window_count > 0 || has_background;
        if (has_windows) {
            if (focus_index >= 0 &&
                (focus_index >= static_cast<int>(kMaxWindows) ||
                 !windows[focus_index].in_use)) {
                focus_index = -1;
            }
            if (focus_index < 0 && window_count == 0 && has_background) {
                focus_index = background_index;
            }
        } else {
            focus_index = -1;
            drag_index = -1;
            if (had_windows) {
                force_full_redraw = true;
            }
        }

        uint32_t cursor_color = lattice::pack_color(info, 255, 255, 255);
        bool cursor_dirty = (cursor_x != prev_x || cursor_y != prev_y);
        bool did_render = false;

        if (has_windows) {
            if (scene_dirty) {
                bool do_full = force_full_redraw || !dirty_rect_valid;
                if (do_full) {
                    lattice::copy_bytes(frame, background, frame_bytes);
                    if (background_index >= 0 &&
                        background_index < static_cast<int>(kMaxWindows) &&
                        windows[background_index].in_use) {
                        blit_window(frame,
                                    info,
                                    bytes_per_pixel,
                                    windows[background_index]);
                    }
                    for (size_t i = 0; i < kMaxWindows; ++i) {
                        if (windows[i].in_use && !windows[i].is_background) {
                            blit_window(frame,
                                        info,
                                        bytes_per_pixel,
                                        windows[i]);
                            bool focused =
                                static_cast<int>(i) == focus_index;
                            uint32_t title_color_use =
                                focused ? title_focus_color : title_color;
                            draw_window_decor(frame,
                                              info,
                                              bytes_per_pixel,
                                              windows[i],
                                              border_color,
                                              title_color_use,
                                              close_fill,
                                              close_border);
                        }
                    }
                    if (use_mapping) {
                        lattice::copy_bytes(fb_ptr, frame, frame_bytes);
                        descriptor_defs::FramebufferRect cursor_update{};
                        if (compute_dirty_rect(info,
                                               prev_x,
                                               prev_y,
                                               cursor_x,
                                               cursor_y,
                                               cursor_update)) {
                            render_cursor_region_mapped(fb_ptr,
                                                        frame,
                                                        info,
                                                        bytes_per_pixel,
                                                        cursor_update,
                                                        cursor_x,
                                                        cursor_y,
                                                        cursor_color);
                        } else {
                            draw_cursor(fb_ptr,
                                        info,
                                        bytes_per_pixel,
                                        cursor_x,
                                        cursor_y,
                                        cursor_color);
                        }
                        framebuffer_present(static_cast<uint32_t>(fb), nullptr);
                    } else {
                        descriptor_write(static_cast<uint32_t>(fb),
                                         frame,
                                         frame_bytes,
                                         0);
                        render_cursor_region(static_cast<uint32_t>(fb),
                                             frame,
                                             info,
                                             bytes_per_pixel,
                                             cursor_x,
                                             cursor_y,
                                             cursor_x,
                                             cursor_y,
                                             cursor_color,
                                             row_buffer,
                                             row_buffer_bytes);
                    }
                } else {
                    copy_rect(frame,
                              background,
                              info,
                              bytes_per_pixel,
                              dirty_rect);
                    if (background_index >= 0 &&
                        background_index < static_cast<int>(kMaxWindows) &&
                        windows[background_index].in_use) {
                        descriptor_defs::FramebufferRect rect{};
                        if (window_rect(windows[background_index], info, rect) &&
                            rect_intersects(rect, dirty_rect)) {
                            blit_window_clipped(frame,
                                                info,
                                                bytes_per_pixel,
                                                windows[background_index],
                                                dirty_rect);
                        }
                    }
                    for (size_t i = 0; i < kMaxWindows; ++i) {
                        if (!windows[i].in_use || windows[i].is_background) {
                            continue;
                        }
                        descriptor_defs::FramebufferRect rect{};
                        if (!window_rect(windows[i], info, rect) ||
                            !rect_intersects(rect, dirty_rect)) {
                            continue;
                        }
                        blit_window_clipped(frame,
                                            info,
                                            bytes_per_pixel,
                                            windows[i],
                                            dirty_rect);
                        bool focused =
                            static_cast<int>(i) == focus_index;
                        uint32_t title_color_use =
                            focused ? title_focus_color : title_color;
                        draw_window_decor_clipped(frame,
                                                  info,
                                                  bytes_per_pixel,
                                                  windows[i],
                                                  border_color,
                                                  title_color_use,
                                                  close_fill,
                                                  dirty_rect);
                    }
                    if (use_mapping) {
                        copy_rect(fb_ptr,
                                  frame,
                                  info,
                                  bytes_per_pixel,
                                  dirty_rect);
                        descriptor_defs::FramebufferRect present_rect =
                            dirty_rect;
                        descriptor_defs::FramebufferRect cursor_update{};
                        if (compute_dirty_rect(info,
                                               prev_x,
                                               prev_y,
                                               cursor_x,
                                               cursor_y,
                                               cursor_update)) {
                            render_cursor_region_mapped(fb_ptr,
                                                        frame,
                                                        info,
                                                        bytes_per_pixel,
                                                        cursor_update,
                                                        cursor_x,
                                                        cursor_y,
                                                        cursor_color);
                            union_rect(present_rect, cursor_update);
                        } else {
                            draw_cursor(fb_ptr,
                                        info,
                                        bytes_per_pixel,
                                        cursor_x,
                                        cursor_y,
                                        cursor_color);
                            descriptor_defs::FramebufferRect cursor_region{};
                            if (cursor_rect(info,
                                            cursor_x,
                                            cursor_y,
                                            cursor_region)) {
                                union_rect(present_rect, cursor_region);
                            }
                        }
                        framebuffer_present(static_cast<uint32_t>(fb),
                                            &present_rect);
                    } else {
                        size_t row_bytes =
                            static_cast<size_t>(dirty_rect.width) *
                            bytes_per_pixel;
                        for (uint32_t row = 0;
                             row < dirty_rect.height;
                             ++row) {
                            size_t offset =
                                static_cast<size_t>(dirty_rect.y + row) *
                                    info.pitch +
                                static_cast<size_t>(dirty_rect.x) *
                                    bytes_per_pixel;
                            descriptor_write(static_cast<uint32_t>(fb),
                                             frame + offset,
                                             row_bytes,
                                             offset);
                        }
                        render_cursor_region(static_cast<uint32_t>(fb),
                                             frame,
                                             info,
                                             bytes_per_pixel,
                                             prev_x,
                                             prev_y,
                                             cursor_x,
                                             cursor_y,
                                             cursor_color,
                                             row_buffer,
                                             row_buffer_bytes);
                    }
                }
                scene_dirty = false;
                dirty_rect_valid = false;
                force_full_redraw = false;
                did_render = true;
            } else if (cursor_dirty) {
                if (use_mapping) {
                    descriptor_defs::FramebufferRect rect{};
                    if (compute_dirty_rect(info,
                                           prev_x,
                                           prev_y,
                                           cursor_x,
                                           cursor_y,
                                           rect)) {
                        render_cursor_region_mapped(fb_ptr,
                                                    frame,
                                                    info,
                                                    bytes_per_pixel,
                                                    rect,
                                                    cursor_x,
                                                    cursor_y,
                                                    cursor_color);
                        framebuffer_present(static_cast<uint32_t>(fb), &rect);
                    }
                } else {
                    render_cursor_region(static_cast<uint32_t>(fb),
                                         frame,
                                         info,
                                         bytes_per_pixel,
                                         prev_x,
                                         prev_y,
                                         cursor_x,
                                         cursor_y,
                                         cursor_color,
                                         row_buffer,
                                         row_buffer_bytes);
                }
                did_render = true;
            }
        } else {
            if (force_full_redraw) {
                lattice::copy_bytes(frame, background, frame_bytes);
                if (use_mapping) {
                    lattice::copy_bytes(fb_ptr, frame, frame_bytes);
                    draw_cursor(fb_ptr,
                                info,
                                bytes_per_pixel,
                                cursor_x,
                                cursor_y,
                                cursor_color);
                    framebuffer_present(static_cast<uint32_t>(fb), nullptr);
                } else {
                    descriptor_write(static_cast<uint32_t>(fb),
                                     frame,
                                     frame_bytes,
                                     0);
                    render_cursor_region(static_cast<uint32_t>(fb),
                                         frame,
                                         info,
                                         bytes_per_pixel,
                                         cursor_x,
                                         cursor_y,
                                         cursor_x,
                                         cursor_y,
                                         cursor_color,
                                         row_buffer,
                                         row_buffer_bytes);
                }
                force_full_redraw = false;
                did_render = true;
            } else if (cursor_dirty) {
                if (use_mapping) {
                    descriptor_defs::FramebufferRect rect{};
                    if (compute_dirty_rect(info,
                                           prev_x,
                                           prev_y,
                                           cursor_x,
                                           cursor_y,
                                           rect)) {
                        render_cursor_region_mapped(fb_ptr,
                                                    frame,
                                                    info,
                                                    bytes_per_pixel,
                                                    rect,
                                                    cursor_x,
                                                    cursor_y,
                                                    cursor_color);
                        framebuffer_present(static_cast<uint32_t>(fb), &rect);
                    }
                } else {
                    render_cursor_region(static_cast<uint32_t>(fb),
                                         frame,
                                         info,
                                         bytes_per_pixel,
                                         prev_x,
                                         prev_y,
                                         cursor_x,
                                         cursor_y,
                                         cursor_color,
                                         row_buffer,
                                         row_buffer_bytes);
                }
                did_render = true;
            }
        }

        if (did_render) {
            prev_x = cursor_x;
            prev_y = cursor_y;
        }
        had_windows = has_windows;

        yield();
    }

    descriptor_close(static_cast<uint32_t>(keyboard));
    descriptor_close(static_cast<uint32_t>(mouse));
    descriptor_close(static_cast<uint32_t>(fb));
    return 0;
}
