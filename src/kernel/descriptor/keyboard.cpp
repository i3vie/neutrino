#include "../descriptor.hpp"

#include "../../drivers/input/keyboard.hpp"

namespace descriptor {

namespace descriptor_keyboard {

int64_t keyboard_read(process::Process&,
                      DescriptorEntry&,
                      uint64_t user_address,
                      uint64_t length,
                      uint64_t offset) {
    if (offset != 0) {
        return -1;
    }
    if (length == 0) {
        return 0;
    }
    auto* buffer = reinterpret_cast<char*>(user_address);
    if (buffer == nullptr) {
        return -1;
    }
    size_t to_read = static_cast<size_t>(length);
    size_t read_count = keyboard::read(buffer, to_read);
    return static_cast<int64_t>(read_count);
}

int64_t keyboard_write(process::Process&,
                       DescriptorEntry&,
                       uint64_t,
                       uint64_t,
                       uint64_t) {
    return -1;
}

const Ops kKeyboardOps{
    .read = keyboard_read,
    .write = keyboard_write,
    .get_property = nullptr,
    .set_property = nullptr,
};

bool open_keyboard(process::Process&,
                   uint64_t,
                   uint64_t,
                   uint64_t,
                   Allocation& alloc) {
    keyboard::init();
    alloc.type = kTypeKeyboard;
    alloc.flags = static_cast<uint64_t>(Flag::Readable);
    alloc.extended_flags = 0;
    alloc.has_extended_flags = false;
    alloc.object = nullptr;
    alloc.close = nullptr;
    alloc.name = "keyboard";
    alloc.ops = &kKeyboardOps;
    return true;
}

}  // namespace descriptor_keyboard

bool register_keyboard_descriptor() {
    return register_type(kTypeKeyboard, descriptor_keyboard::open_keyboard, &descriptor_keyboard::kKeyboardOps);
}

}  // namespace descriptor
