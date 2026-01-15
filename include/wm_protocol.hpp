#pragma once

#include <stdint.h>

namespace wm {

constexpr uint32_t kRegistryMagic = 0x574d3031;  // "WM01"
constexpr uint16_t kRegistryVersion = 1;

struct Registry {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    uint32_t server_pipe_id;
};

enum class MessageType : uint32_t {
    CreateWindow = 1,
};

enum class ClientMessage : uint8_t {
    Present = 1,
};

enum class ServerMessage : uint8_t {
    Mouse = 0xFE,
    Close = 0xFF,
};

struct __attribute__((packed)) ServerMouseMessage {
    uint8_t type;
    uint8_t buttons;
    uint16_t x;
    uint16_t y;
};

struct CreateRequest {
    uint32_t type;
    uint32_t reply_pipe_id;
    uint32_t width;
    uint32_t height;
    uint32_t flags;
    char title[32];
};

constexpr uint32_t kWindowFlagBackground = 1u << 0;

struct PixelFormat {
    uint16_t bpp;
    uint8_t red_mask_size;
    uint8_t red_mask_shift;
    uint8_t green_mask_size;
    uint8_t green_mask_shift;
    uint8_t blue_mask_size;
    uint8_t blue_mask_shift;
};

struct CreateResponse {
    uint32_t type;
    int32_t status;
    uint32_t window_id;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    int32_t x;
    int32_t y;
    uint32_t in_pipe_id;
    uint32_t out_pipe_id;
    char shm_name[48];
    PixelFormat format;
};

}  // namespace wm
