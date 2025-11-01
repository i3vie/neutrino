#include "descriptor.hpp"

#include "../drivers/console/console.hpp"
#include "../drivers/input/keyboard.hpp"
#include "../drivers/log/logging.hpp"
#include "../drivers/serial/serial.hpp"
#include "process.hpp"

namespace descriptor {

namespace {

int64_t console_read(process::Process&,
                     Entry&,
                     uint64_t,
                     uint64_t,
                     uint64_t) {
    return -1;
}

int64_t console_write(process::Process&,
                      Entry& entry,
                      uint64_t user_address,
                      uint64_t length,
                      uint64_t offset) {
    if (offset != 0) {
        return -1;
    }
    auto* console = static_cast<Console*>(entry.object);
    if (console == nullptr) {
        return -1;
    }
    const char* data = reinterpret_cast<const char*>(user_address);
    if (data == nullptr || length == 0) {
        return 0;
    }
    size_t to_write = static_cast<size_t>(length);
    for (size_t i = 0; i < to_write; ++i) {
        console->putc(data[i]);
    }
    return static_cast<int64_t>(to_write);
}

const Ops kConsoleOps{
    .read = console_read,
    .write = console_write,
};

process::Process* g_console_owner = nullptr;
size_t g_console_refcount = 0;

void close_console(void*) {
    if (g_console_refcount > 0) {
        --g_console_refcount;
    }
    if (g_console_refcount == 0) {
        g_console_owner = nullptr;
    }
}

bool open_console(process::Process& proc,
                  uint64_t,
                  uint64_t,
                  uint64_t,
                  Allocation& alloc) {
    if (kconsole == nullptr) {
        return false;
    }
    if (g_console_owner != nullptr && g_console_owner != &proc) {
        return false;
    }
    g_console_owner = &proc;
    ++g_console_refcount;
    alloc.info = make_info(kTypeConsole, CapabilityWritable);
    alloc.object = kconsole;
    alloc.close = close_console;
    alloc.ops = &kConsoleOps;
    return true;
}

int64_t serial_read(process::Process&,
                    Entry&,
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
    size_t read_count = serial::read(buffer, to_read);
    return static_cast<int64_t>(read_count);
}

int64_t serial_write(process::Process&,
                     Entry&,
                     uint64_t user_address,
                     uint64_t length,
                     uint64_t offset) {
    if (offset != 0) {
        return -1;
    }
    const char* data = reinterpret_cast<const char*>(user_address);
    if (data == nullptr || length == 0) {
        return 0;
    }
    size_t to_write = static_cast<size_t>(length);
    serial::write(data, to_write);
    return static_cast<int64_t>(to_write);
}

const Ops kSerialOps{
    .read = serial_read,
    .write = serial_write,
};

bool open_serial(process::Process&,
                 uint64_t,
                 uint64_t,
                 uint64_t,
                 Allocation& alloc) {
    serial::init();
    alloc.info = make_info(kTypeSerial, CapabilityReadable | CapabilityWritable);
    alloc.object = nullptr;
    alloc.close = nullptr;
    alloc.ops = &kSerialOps;
    return true;
}

int64_t keyboard_read(process::Process&,
                      Entry&,
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
                       Entry&,
                       uint64_t,
                       uint64_t,
                       uint64_t) {
    return -1;
}

const Ops kKeyboardOps{
    .read = keyboard_read,
    .write = keyboard_write,
};

bool open_keyboard(process::Process&,
                   uint64_t,
                   uint64_t,
                   uint64_t,
                   Allocation& alloc) {
    keyboard::init();
    alloc.info = make_info(kTypeKeyboard, CapabilityReadable);
    alloc.object = nullptr;
    alloc.close = nullptr;
    alloc.ops = &kKeyboardOps;
    return true;
}

}  // namespace

void register_builtin_types() {
    if (!register_type(kTypeConsole, open_console, &kConsoleOps)) {
        log_message(LogLevel::Warn,
                    "Descriptor: failed to register console descriptor type");
    }
    if (!register_type(kTypeSerial, open_serial, &kSerialOps)) {
        log_message(LogLevel::Warn,
                    "Descriptor: failed to register serial descriptor type");
    }
    if (!register_type(kTypeKeyboard, open_keyboard, &kKeyboardOps)) {
        log_message(LogLevel::Warn,
                    "Descriptor: failed to register keyboard descriptor type");
    }
}

}  // namespace descriptor
