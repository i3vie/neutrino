#include <stddef.h>
#include <stdint.h>

#include "descriptors.hpp"
#include "../crt/syscall.hpp"

namespace {

constexpr size_t kChunkBytes = 4096;

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

}  // namespace

int main(uint64_t, uint64_t) {
    long handle = framebuffer_open();
    if (handle < 0) {
        return 1;
    }

    descriptor_defs::FramebufferInfo info{};
    if (framebuffer_get_info(static_cast<uint32_t>(handle), &info) != 0) {
        descriptor_close(static_cast<uint32_t>(handle));
        return 1;
    }

    if (info.width == 0 || info.height == 0 || info.bpp == 0) {
        descriptor_close(static_cast<uint32_t>(handle));
        return 1;
    }

    uint32_t bytes_per_pixel = (info.bpp + 7u) / 8u;
    if (bytes_per_pixel == 0 || bytes_per_pixel > 4) {
        descriptor_close(static_cast<uint32_t>(handle));
        return 1;
    }

    uint8_t buffer[kChunkBytes];
    uint32_t pixels_per_chunk = static_cast<uint32_t>(kChunkBytes / bytes_per_pixel);
    if (pixels_per_chunk == 0) {
        descriptor_close(static_cast<uint32_t>(handle));
        return 1;
    }

    for (uint32_t y = 0; y < info.height; ++y) {
        uint32_t x = 0;
        while (x < info.width) {
            uint32_t remaining = info.width - x;
            uint32_t pixels = (remaining < pixels_per_chunk)
                                  ? remaining
                                  : pixels_per_chunk;
            size_t bytes = static_cast<size_t>(pixels) * bytes_per_pixel;
            for (uint32_t i = 0; i < pixels; ++i) {
                uint32_t px = x + i;
                uint32_t r = (px * 255u) / info.width;
                uint32_t g = (y * 255u) / info.height;
                uint32_t b = ((px ^ y) & 0xFFu);
                uint32_t rs = scale_channel(r, info.red_mask_size);
                uint32_t gs = scale_channel(g, info.green_mask_size);
                uint32_t bs = scale_channel(b, info.blue_mask_size);
                uint32_t pixel =
                    (rs << info.red_mask_shift) |
                    (gs << info.green_mask_shift) |
                    (bs << info.blue_mask_shift);
                size_t base = static_cast<size_t>(i) * bytes_per_pixel;
                for (uint32_t byte = 0; byte < bytes_per_pixel; ++byte) {
                    buffer[base + byte] =
                        static_cast<uint8_t>((pixel >> (8u * byte)) & 0xFFu);
                }
            }
            uint64_t offset =
                static_cast<uint64_t>(y) * info.pitch +
                static_cast<uint64_t>(x) * bytes_per_pixel;
            descriptor_write(static_cast<uint32_t>(handle),
                             buffer,
                             bytes,
                             offset);
            x += pixels;
        }
    }

    descriptor_close(static_cast<uint32_t>(handle));
    return 0;
}
