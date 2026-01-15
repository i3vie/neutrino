#include "lattice.hpp"

namespace lattice {

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

uint32_t pack_color(const wm::PixelFormat& fmt,
                    uint32_t r,
                    uint32_t g,
                    uint32_t b) {
    uint32_t rs = scale_channel(r, fmt.red_mask_size);
    uint32_t gs = scale_channel(g, fmt.green_mask_size);
    uint32_t bs = scale_channel(b, fmt.blue_mask_size);
    return (rs << fmt.red_mask_shift) |
           (gs << fmt.green_mask_shift) |
           (bs << fmt.blue_mask_shift);
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

void store_pixel(uint8_t* dest,
                 uint32_t bytes_per_pixel,
                 uint32_t pixel) {
    if (dest == nullptr) {
        return;
    }
    for (uint32_t byte = 0; byte < bytes_per_pixel; ++byte) {
        dest[byte] = static_cast<uint8_t>((pixel >> (8u * byte)) & 0xFFu);
    }
}

void write_pixel(uint8_t* buffer,
                 uint32_t stride,
                 uint32_t bytes_per_pixel,
                 uint32_t x,
                 uint32_t y,
                 uint32_t pixel) {
    if (buffer == nullptr) {
        return;
    }
    size_t offset = static_cast<size_t>(y) * stride +
                    static_cast<size_t>(x) * bytes_per_pixel;
    store_pixel(buffer + offset, bytes_per_pixel, pixel);
}

void write_pixel(uint8_t* buffer,
                 const descriptor_defs::FramebufferInfo& info,
                 uint32_t bytes_per_pixel,
                 uint32_t x,
                 uint32_t y,
                 uint32_t pixel) {
    if (buffer == nullptr) {
        return;
    }
    size_t offset = static_cast<size_t>(y) * info.pitch +
                    static_cast<size_t>(x) * bytes_per_pixel;
    store_pixel(buffer + offset, bytes_per_pixel, pixel);
}

void write_pixel_raw(uint8_t* buffer,
                     uint32_t bytes_per_pixel,
                     size_t offset,
                     uint32_t pixel) {
    if (buffer == nullptr) {
        return;
    }
    for (uint32_t byte = 0; byte < bytes_per_pixel; ++byte) {
        buffer[offset + byte] =
            static_cast<uint8_t>((pixel >> (8u * byte)) & 0xFFu);
    }
}

void copy_bytes(uint8_t* dest, const uint8_t* src, size_t count) {
    if (dest == nullptr || src == nullptr || count == 0) {
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        dest[i] = src[i];
    }
}

void fill_rect(uint8_t* frame,
               const descriptor_defs::FramebufferInfo& info,
               uint32_t bytes_per_pixel,
               int32_t x,
               int32_t y,
               uint32_t width,
               uint32_t height,
               uint32_t color) {
    if (frame == nullptr || width == 0 || height == 0) {
        return;
    }
    int32_t left = x;
    int32_t top = y;
    int32_t right = x + static_cast<int32_t>(width);
    int32_t bottom = y + static_cast<int32_t>(height);
    if (right <= 0 || bottom <= 0) {
        return;
    }
    if (left >= static_cast<int32_t>(info.width) ||
        top >= static_cast<int32_t>(info.height)) {
        return;
    }
    if (left < 0) {
        left = 0;
    }
    if (top < 0) {
        top = 0;
    }
    if (right > static_cast<int32_t>(info.width)) {
        right = static_cast<int32_t>(info.width);
    }
    if (bottom > static_cast<int32_t>(info.height)) {
        bottom = static_cast<int32_t>(info.height);
    }
    for (int32_t py = top; py < bottom; ++py) {
        for (int32_t px = left; px < right; ++px) {
            write_pixel(frame,
                        info,
                        bytes_per_pixel,
                        static_cast<uint32_t>(px),
                        static_cast<uint32_t>(py),
                        color);
        }
    }
}

void fill_rect_stride(uint8_t* buffer,
                      uint32_t width,
                      uint32_t height,
                      uint32_t stride,
                      uint32_t bytes_per_pixel,
                      int32_t x,
                      int32_t y,
                      uint32_t rect_width,
                      uint32_t rect_height,
                      uint32_t color) {
    if (buffer == nullptr || rect_width == 0 || rect_height == 0) {
        return;
    }
    int32_t left = x;
    int32_t top = y;
    int32_t right = x + static_cast<int32_t>(rect_width);
    int32_t bottom = y + static_cast<int32_t>(rect_height);
    if (right <= 0 || bottom <= 0) {
        return;
    }
    if (left < 0) {
        left = 0;
    }
    if (top < 0) {
        top = 0;
    }
    if (right > static_cast<int32_t>(width)) {
        right = static_cast<int32_t>(width);
    }
    if (bottom > static_cast<int32_t>(height)) {
        bottom = static_cast<int32_t>(height);
    }
    for (int32_t py = top; py < bottom; ++py) {
        for (int32_t px = left; px < right; ++px) {
            write_pixel(buffer,
                        stride,
                        bytes_per_pixel,
                        static_cast<uint32_t>(px),
                        static_cast<uint32_t>(py),
                        color);
        }
    }
}

}  // namespace lattice
