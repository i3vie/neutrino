#include "hid_keyboard.hpp"

#include "drivers/input/keyboard.hpp"

namespace usb::hid {
namespace {

constexpr uint8_t kHidLetterScancodes[26] = {
    0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26,
    0x32, 0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D,
    0x15, 0x2C
};

bool hid_usage_to_scancode(uint8_t usage, uint8_t& scancode, bool& extended) {
    extended = false;
    if (usage >= 0x04 && usage <= 0x1D) {
        scancode = kHidLetterScancodes[usage - 0x04];
        return true;
    }
    if (usage >= 0x1E && usage <= 0x27) {
        scancode = static_cast<uint8_t>(usage - 0x1E + 0x02);
        return true;
    }
    if (usage >= 0x3A && usage <= 0x43) {
        scancode = static_cast<uint8_t>(usage - 0x3A + 0x3B);
        return true;
    }
    switch (usage) {
        case 0x28: scancode = 0x1C; return true;
        case 0x29: scancode = 0x01; return true;
        case 0x2A: scancode = 0x0E; return true;
        case 0x2B: scancode = 0x0F; return true;
        case 0x2C: scancode = 0x39; return true;
        case 0x2D: scancode = 0x0C; return true;
        case 0x2E: scancode = 0x0D; return true;
        case 0x2F: scancode = 0x1A; return true;
        case 0x30: scancode = 0x1B; return true;
        case 0x31: scancode = 0x2B; return true;
        case 0x33: scancode = 0x27; return true;
        case 0x34: scancode = 0x28; return true;
        case 0x35: scancode = 0x29; return true;
        case 0x36: scancode = 0x33; return true;
        case 0x37: scancode = 0x34; return true;
        case 0x38: scancode = 0x35; return true;
        case 0x39: scancode = 0x3A; return true;
        case 0x44: scancode = 0x57; return true;
        case 0x45: scancode = 0x58; return true;
        case 0x4F: scancode = 0x4D; extended = true; return true;
        case 0x50: scancode = 0x4B; extended = true; return true;
        case 0x51: scancode = 0x50; extended = true; return true;
        case 0x52: scancode = 0x48; extended = true; return true;
        default:
            return false;
    }
}

bool usage_in_list(uint8_t usage, const uint8_t* list, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (list[i] == usage) {
            return true;
        }
    }
    return false;
}

}  // namespace

void init_keyboard_state(KeyboardState& state) {
    for (auto& v : state.last_report) {
        v = 0;
    }
    for (auto& k : state.prev_keys) {
        k = 0;
    }
    state.modifier_state = 0;
    state.caps_lock = false;
}

void handle_keyboard_report(KeyboardState& state,
                            const uint8_t* report,
                            size_t length) {
    if (report == nullptr || length < sizeof(state.last_report)) {
        return;
    }

    uint8_t modifiers = report[0];
    uint8_t prev_modifiers = state.modifier_state;

    auto handle_modifier = [&](uint8_t bit,
                               uint8_t scancode,
                               bool extended) {
        bool was_down = (prev_modifiers & bit) != 0;
        bool now_down = (modifiers & bit) != 0;
        if (was_down == now_down) {
            return;
        }
        keyboard::inject_scancode(scancode, extended, now_down);
    };

    handle_modifier(1u << 0, 0x1D, false);  // Left Ctrl
    handle_modifier(1u << 1, 0x2A, false);  // Left Shift
    handle_modifier(1u << 2, 0x38, false);  // Left Alt
    handle_modifier(1u << 4, 0x1D, true);   // Right Ctrl
    handle_modifier(1u << 5, 0x36, false);  // Right Shift
    handle_modifier(1u << 6, 0x38, true);   // Right Alt

    const uint8_t* new_keys = report + 2;
    size_t key_count = 6;

    for (size_t i = 0; i < key_count; ++i) {
        uint8_t usage = state.prev_keys[i];
        if (usage == 0) {
            continue;
        }
        if (!usage_in_list(usage, new_keys, key_count)) {
            uint8_t scancode = 0;
            bool extended = false;
            if (hid_usage_to_scancode(usage, scancode, extended)) {
                keyboard::inject_scancode(scancode, extended, false);
            }
        }
    }

    for (size_t i = 0; i < key_count; ++i) {
        uint8_t usage = new_keys[i];
        if (usage == 0) {
            continue;
        }
        if (!usage_in_list(usage, state.prev_keys, key_count)) {
            uint8_t scancode = 0;
            bool extended = false;
            if (hid_usage_to_scancode(usage, scancode, extended)) {
                keyboard::inject_scancode(scancode, extended, true);
            }
        }
    }

    for (size_t i = 0; i < key_count; ++i) {
        state.prev_keys[i] = new_keys[i];
    }
    state.modifier_state = modifiers;

    size_t copy_len = length;
    if (copy_len > sizeof(state.last_report)) {
        copy_len = sizeof(state.last_report);
    }
    for (size_t i = 0; i < copy_len; ++i) {
        state.last_report[i] = report[i];
    }
}

}  // namespace usb::hid
