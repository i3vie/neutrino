#pragma once

#include <stdint.h>
#include <stddef.h>

namespace descriptor_defs {

enum class Type : uint16_t {
    Console     = 0x001,
    Serial      = 0x002,
    Keyboard    = 0x003,
    Mouse       = 0x004,
    Framebuffer = 0x010,
    BlockDevice = 0x020,
    Pipe        = 0x030,
    SharedMemory = 0x040,
};

enum class Property : uint32_t {
    CommonName        = 0x00000001,
    FramebufferInfo   = 0x00010001,
    FramebufferPresent= 0x00010002,
    BlockGeometry     = 0x00020001,
    SharedMemoryInfo  = 0x00030001,
    PipeInfo          = 0x00040001,
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

struct FramebufferRect {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
};

struct BlockGeometry {
    uint64_t sector_size;
    uint64_t sector_count;
};

struct SharedMemoryInfo {
    uint64_t base;
    uint64_t length;
};

struct PipeInfo {
    uint32_t id;
    uint32_t flags;
};

struct MouseEvent {
    int8_t dx;
    int8_t dy;
    uint8_t buttons;
    uint8_t reserved;
};

}  // namespace descriptor_defs
