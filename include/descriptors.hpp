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
    Vty         = 0x050,
};

enum class Flag : uint64_t {
    Readable    = 1ull << 0,
    Writable    = 1ull << 1,
    Seekable    = 1ull << 2,
    Mappable    = 1ull << 3,
    Async       = 1ull << 8,
    EventSource = 1ull << 9,
    Device      = 1ull << 10,
    Block       = 1ull << 11,
};

enum class Property : uint32_t {
    CommonName        = 0x00000001,
    FramebufferInfo   = 0x00010001,
    FramebufferPresent= 0x00010002,
    BlockGeometry     = 0x00020001,
    SharedMemoryInfo  = 0x00030001,
    PipeInfo          = 0x00040001,
    VtyInfo           = 0x00050001,
    VtyCells          = 0x00050002,
    VtyInjectInput    = 0x00050003,
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

enum class VtyOpen : uint64_t {
    Attach = 1ull << 0,
};

struct VtyInfo {
    uint32_t id;
    uint32_t cols;
    uint32_t rows;
    uint32_t cursor_x;
    uint32_t cursor_y;
    uint32_t flags;
    uint32_t cell_bytes;
};

struct VtyCell {
    uint8_t ch;
    uint8_t fg;
    uint8_t bg;
    uint8_t flags;
};

}  // namespace descriptor_defs
