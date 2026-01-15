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
    MenuUpdate = 2,
    MenuInvoke = 3,
};

enum class ServerMessage : uint8_t {
    MenuCommand = 0xFB,
    MenuBar = 0xFC,
    Key = 0xFD,
    Mouse = 0xFE,
    Close = 0xFF,
};

constexpr uint8_t kMenuLabelSize = 16;
constexpr uint8_t kMenuItemLabelSize = 24;
constexpr uint8_t kMenuMaxItems = 8;
constexpr uint8_t kMenuMaxMenus = 4;

struct MenuItem {
    char label[kMenuItemLabelSize];
    uint32_t id;
};

struct Menu {
    char label[kMenuLabelSize];
    uint8_t item_count;
    uint8_t reserved[3];
    MenuItem items[kMenuMaxItems];
};

struct MenuBar {
    uint8_t menu_count;
    uint8_t reserved[3];
    Menu menus[kMenuMaxMenus];
};

struct __attribute__((packed)) ClientMenuUpdate {
    uint8_t type;
    MenuBar bar;
};

struct __attribute__((packed)) ClientMenuInvoke {
    uint8_t type;
    uint8_t menu_index;
    uint8_t item_index;
    uint8_t reserved;
};

struct __attribute__((packed)) ServerMenuCommand {
    uint8_t type;
    uint8_t reserved[3];
    uint32_t id;
};

struct __attribute__((packed)) ServerMenuBarMessage {
    uint8_t type;
    char title[32];
    MenuBar bar;
};

struct __attribute__((packed)) ServerKeyMessage {
    uint8_t type;
    uint8_t scancode;
    uint8_t flags;
    uint8_t mods;
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
