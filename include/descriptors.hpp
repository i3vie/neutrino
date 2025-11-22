#pragma once

#include <stdint.h>
#include <stddef.h>

namespace descriptor_defs {

enum class Type : uint16_t {
    Console     = 0x001,
    Serial      = 0x002,
    Keyboard    = 0x003,
    Framebuffer = 0x010,
    BlockDevice = 0x020,
    Pipe        = 0x030,
};

enum class Property : uint32_t {
    CommonName        = 0x00000001,
    FramebufferInfo   = 0x00010001,
    BlockGeometry     = 0x00020001,
};

struct FramebufferInfo {
    uint64_t physical_base;
    uint64_t virtual_base;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint16_t bpp;
    uint8_t memory_model;
    uint8_t reserved;
    uint8_t red_mask_size;
    uint8_t red_mask_shift;
    uint8_t green_mask_size;
    uint8_t green_mask_shift;
    uint8_t blue_mask_size;
    uint8_t blue_mask_shift;
};

struct BlockGeometry {
    uint64_t sector_size;
    uint64_t sector_count;
};

}  // namespace descriptor_defs
