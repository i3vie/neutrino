#pragma once

#include <stddef.h>
#include <stdint.h>

#include "descriptors.hpp"

namespace keyboard {

constexpr uint8_t kScancodeLeft = 0x4B;
constexpr uint8_t kScancodeRight = 0x4D;
constexpr uint8_t kScancodeUp = 0x48;
constexpr uint8_t kScancodeDown = 0x50;

inline constexpr char kScancodeMap[129] = {
    0,
    27,  '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b', '\t',
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0,  'a',
    's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0,  '\\', 'z', 'x',
    'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,   '*', 0,   ' ', 0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   '7', '8', '9', '-', '4', '5', '6', '+',
    '1', '2', '3', '0', '.', 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0
};

inline constexpr char kScancodeShiftMap[129] = {
    0,
    27,  '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b', '\t',
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0,  'A',
    'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0,  '|', 'Z', 'X',
    'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,   '*', 0,   ' ', 0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   '7', '8', '9', '-', '4', '5', '6', '+',
    '1', '2', '3', '0', '.', 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0
};

inline char scancode_to_char(uint8_t scancode, uint8_t mods) {
    if (scancode >= sizeof(kScancodeMap)) {
        return 0;
    }
    char base = kScancodeMap[scancode];
    if (base == 0) {
        return 0;
    }
    bool shift = (mods & descriptor_defs::kKeyboardModShift) != 0;
    bool caps = (mods & descriptor_defs::kKeyboardModCaps) != 0;
    bool alphabetic = (base >= 'a' && base <= 'z');
    if (alphabetic) {
        if (shift ^ caps) {
            base = static_cast<char>(base - 'a' + 'A');
        }
    } else if (shift) {
        char shifted = kScancodeShiftMap[scancode];
        if (shifted != 0) {
            base = shifted;
        }
    }
    return base;
}

inline bool is_pressed(const descriptor_defs::KeyboardEvent& event) {
    return (event.flags & descriptor_defs::kKeyboardFlagPressed) != 0;
}

inline bool is_extended(const descriptor_defs::KeyboardEvent& event) {
    return (event.flags & descriptor_defs::kKeyboardFlagExtended) != 0;
}

inline bool is_arrow_key(const descriptor_defs::KeyboardEvent& event,
                         int32_t& dx,
                         int32_t& dy) {
    dx = 0;
    dy = 0;
    if (!is_pressed(event) || !is_extended(event)) {
        return false;
    }
    switch (event.scancode) {
        case kScancodeLeft:
            dx = -1;
            return true;
        case kScancodeRight:
            dx = 1;
            return true;
        case kScancodeUp:
            dy = -1;
            return true;
        case kScancodeDown:
            dy = 1;
            return true;
        default:
            return false;
    }
}

}  // namespace keyboard
