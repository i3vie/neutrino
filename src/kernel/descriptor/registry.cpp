#include "../descriptor.hpp"

#include "../../drivers/log/logging.hpp"

namespace descriptor {

bool register_console_descriptor();
bool register_serial_descriptor();
bool register_keyboard_descriptor();
bool register_mouse_descriptor();
bool register_pipe_descriptor();
bool register_framebuffer_descriptor();
bool register_block_device_descriptor();
bool register_shared_memory_descriptor();

void register_builtin_types() {
    reset_block_device_registry();

    if (!register_console_descriptor()) {
        log_message(LogLevel::Warn,
                    "Descriptor: failed to register console descriptor type");
    }
    if (!register_serial_descriptor()) {
        log_message(LogLevel::Warn,
                    "Descriptor: failed to register serial descriptor type");
    }
    if (!register_keyboard_descriptor()) {
        log_message(LogLevel::Warn,
                    "Descriptor: failed to register keyboard descriptor type");
    }
    if (!register_mouse_descriptor()) {
        log_message(LogLevel::Warn,
                    "Descriptor: failed to register mouse descriptor type");
    }
    if (!register_pipe_descriptor()) {
        log_message(LogLevel::Warn,
                    "Descriptor: failed to register pipe descriptor type");
    }
    if (!register_framebuffer_descriptor()) {
        log_message(LogLevel::Warn,
                    "Descriptor: failed to register framebuffer descriptor type");
    }
    if (!register_block_device_descriptor()) {
        log_message(LogLevel::Warn,
                    "Descriptor: failed to register block device descriptor type");
    }
    if (!register_shared_memory_descriptor()) {
        log_message(LogLevel::Warn,
                    "Descriptor: failed to register shared memory descriptor type");
    }
}

}  // namespace descriptor
