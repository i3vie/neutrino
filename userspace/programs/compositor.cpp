#include <stddef.h>
#include <stdint.h>

#include "descriptors.hpp"
#include "../crt/syscall.hpp"

namespace {

constexpr uint32_t kSlot = 1;
constexpr uint32_t kCursorSize = 7;

uint32_t scale_channel(uint32_t value, uint8_t mask_size) {
    if (mask_size == 0) {
        return 0;
    }
    if (mask_size >= 8) {
        return value;
    }
    uint32_t max_value = (1u << mask_size) - 1u;
    return (value * max_value + 127u) / 255u;
}

uint32_t pack_color(const descriptor_defs::FramebufferInfo& info,
                    uint32_t r,
                    uint32_t g,
                    uint32_t b) {
    uint32_t rs = scale_channel(r, info.red_mask_size);
    uint32_t gs = scale_channel(g, info.green_mask_size);
    uint32_t bs = scale_channel(b, info.blue_mask_size);
    return (rs << info.red_mask_shift) |
           (gs << info.green_mask_shift) |
           (bs << info.blue_mask_shift);
}

void write_pixel(uint8_t* frame,
                 const descriptor_defs::FramebufferInfo& info,
                 uint32_t bytes_per_pixel,
                 uint32_t x,
                 uint32_t y,
                 uint32_t pixel) {
    size_t offset = static_cast<size_t>(y) * info.pitch +
                    static_cast<size_t>(x) * bytes_per_pixel;
    for (uint32_t byte = 0; byte < bytes_per_pixel; ++byte) {
        frame[offset + byte] =
            static_cast<uint8_t>((pixel >> (8u * byte)) & 0xFFu);
    }
}

void write_pixel_raw(uint8_t* buffer,
                     uint32_t bytes_per_pixel,
                     size_t offset,
                     uint32_t pixel) {
    for (uint32_t byte = 0; byte < bytes_per_pixel; ++byte) {
        buffer[offset + byte] =
            static_cast<uint8_t>((pixel >> (8u * byte)) & 0xFFu);
    }
}

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
            uint32_t pixel = pack_color(info, r, g, b);
            write_pixel(frame, info, bytes_per_pixel, x, y, pixel);
        }
    }
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
                write_pixel_raw(dest, bytes_per_pixel, offset, color);
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
                write_pixel_raw(dest, bytes_per_pixel, offset, color);
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
                write_pixel_raw(row_buffer, bytes_per_pixel, offset, color);
            }
        }

        int32_t v_offset = y - cursor_y;
        if (v_offset >= -half && v_offset <= half) {
            if (cursor_x >= left && cursor_x <= right) {
                size_t offset =
                    static_cast<size_t>(cursor_x - left) * bytes_per_pixel;
                write_pixel_raw(row_buffer, bytes_per_pixel, offset, color);
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

    uint8_t* fb_ptr = reinterpret_cast<uint8_t*>(info.virtual_base);
    bool use_mapping = fb_ptr != nullptr;

    change_slot(kSlot);

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
    bool dirty = true;

    render_background(frame, info, bytes_per_pixel);
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
                int32_t nx = cursor_x + events[i].dx;
                int32_t ny = cursor_y - events[i].dy;
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
                dirty = true;
            }
        }

        uint8_t key = 0;
        long kread = descriptor_read(static_cast<uint32_t>(keyboard),
                                     &key,
                                     1);
        if (kread > 0) {
            if (key == 27 || key == 'q') {
                break;
            }
        }

        if (dirty) {
            uint32_t cursor_color = pack_color(info, 255, 255, 255);
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
            prev_x = cursor_x;
            prev_y = cursor_y;
            dirty = false;
        }

        yield();
    }

    descriptor_close(static_cast<uint32_t>(keyboard));
    descriptor_close(static_cast<uint32_t>(mouse));
    descriptor_close(static_cast<uint32_t>(fb));
    return 0;
}
