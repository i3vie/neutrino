#pragma once
#include <stddef.h>
#include <stdint.h>

namespace usb::hid {

struct KeyboardState {
    uint8_t last_report[8];
    uint8_t prev_keys[6];
    uint8_t modifier_state;
    bool caps_lock;
};

void init_keyboard_state(KeyboardState& state);

void handle_keyboard_report(KeyboardState& state,
                            const uint8_t* report,
                            size_t length);

}  // namespace usb::hid
