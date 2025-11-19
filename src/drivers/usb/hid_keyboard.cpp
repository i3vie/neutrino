#include "hid_keyboard.hpp"

#include "drivers/input/keyboard.hpp"

namespace usb::hid {
namespace {

char hid_usage_to_char(uint8_t usage, bool shift, bool caps) {
    if (usage >= 0x04 && usage <= 0x1D) {
        char base = static_cast<char>('a' + (usage - 0x04));
        bool upper = shift ^ caps;
        return upper ? static_cast<char>(base - 'a' + 'A') : base;
    }

    switch (usage) {
        case 0x1E: return shift ? '!' : '1';
        case 0x1F: return shift ? '@' : '2';
        case 0x20: return shift ? '#' : '3';
        case 0x21: return shift ? '$' : '4';
        case 0x22: return shift ? '%' : '5';
        case 0x23: return shift ? '^' : '6';
        case 0x24: return shift ? '&' : '7';
        case 0x25: return shift ? '*' : '8';
        case 0x26: return shift ? '(' : '9';
        case 0x27: return shift ? ')' : '0';
        case 0x28: return '\n';
        case 0x29: return 27;
        case 0x2A: return '\b';
        case 0x2B: return '\t';
        case 0x2C: return ' ';
        case 0x2D: return shift ? '_' : '-';
        case 0x2E: return shift ? '+' : '=';
        case 0x2F: return shift ? '{' : '[';
        case 0x30: return shift ? '}' : ']';
        case 0x31: return shift ? '|' : '\\';
        case 0x33: return shift ? ':' : ';';
        case 0x34: return shift ? '"' : '\'';
        case 0x35: return shift ? '~' : '`';
        case 0x36: return shift ? '<' : ',';
        case 0x37: return shift ? '>' : '.';
        case 0x38: return shift ? '?' : '/';
        default:
            return 0;
    }
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
    bool shift = (modifiers & ((1u << 1) | (1u << 5))) != 0;

    for (size_t i = 2; i < sizeof(state.last_report); ++i) {
        if (report[i] != 0x39) {
            continue;
        }
        bool already = false;
        for (uint8_t prev : state.prev_keys) {
            if (prev == 0x39) {
                already = true;
                break;
            }
        }
        if (!already) {
            state.caps_lock = !state.caps_lock;
        }
    }

    for (size_t i = 2; i < sizeof(state.last_report); ++i) {
        uint8_t usage = report[i];
        if (usage == 0 || usage == 0x39) {
            continue;
        }
        bool repeat = false;
        for (uint8_t prev : state.prev_keys) {
            if (prev == usage) {
                repeat = true;
                break;
            }
        }
        if (repeat) {
            continue;
        }

        char ch = hid_usage_to_char(usage, shift, state.caps_lock);
        if (ch != 0) {
            keyboard::inject_char(ch);
        }
    }

    for (size_t i = 0; i < 6; ++i) {
        state.prev_keys[i] = report[i + 2];
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
