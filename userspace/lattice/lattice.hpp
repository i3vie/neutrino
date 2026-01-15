#pragma once

#include <stddef.h>
#include <stdint.h>

#include "descriptors.hpp"
#include "wm_protocol.hpp"

namespace lattice {

uint32_t scale_channel(uint32_t value, uint8_t mask_size);

uint32_t pack_color(const wm::PixelFormat& fmt,
                    uint32_t r,
                    uint32_t g,
                    uint32_t b);

uint32_t pack_color(const descriptor_defs::FramebufferInfo& info,
                    uint32_t r,
                    uint32_t g,
                    uint32_t b);

void store_pixel(uint8_t* dest,
                 uint32_t bytes_per_pixel,
                 uint32_t pixel);

void write_pixel(uint8_t* buffer,
                 uint32_t stride,
                 uint32_t bytes_per_pixel,
                 uint32_t x,
                 uint32_t y,
                 uint32_t pixel);

void write_pixel(uint8_t* buffer,
                 const descriptor_defs::FramebufferInfo& info,
                 uint32_t bytes_per_pixel,
                 uint32_t x,
                 uint32_t y,
                 uint32_t pixel);

void write_pixel_raw(uint8_t* buffer,
                     uint32_t bytes_per_pixel,
                     size_t offset,
                     uint32_t pixel);

void copy_bytes(uint8_t* dest, const uint8_t* src, size_t count);

void fill_rect(uint8_t* frame,
               const descriptor_defs::FramebufferInfo& info,
               uint32_t bytes_per_pixel,
               int32_t x,
               int32_t y,
               uint32_t width,
               uint32_t height,
               uint32_t color);

}  // namespace lattice
