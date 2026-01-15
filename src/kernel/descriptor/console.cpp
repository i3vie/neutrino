#include "../descriptor.hpp"

#include "../../drivers/console/console.hpp"
#include "../process.hpp"

namespace descriptor {

namespace console_descriptor {

int64_t console_read(process::Process&,
                     DescriptorEntry&,
                     uint64_t,
                     uint64_t,
                     uint64_t) {
    return -1;
}

int64_t console_write(process::Process& proc,
                      DescriptorEntry& entry,
                      uint64_t user_address,
                      uint64_t length,
                      uint64_t offset) {
    if (offset != 0) {
        return -1;
    }
    if (proc.vty_id != 0) {
        const char* data = reinterpret_cast<const char*>(user_address);
        if (data == nullptr || length == 0) {
            return 0;
        }
        if (vty_write(proc.vty_id, data, static_cast<size_t>(length))) {
            return static_cast<int64_t>(length);
        }
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
    .get_property = nullptr,
    .set_property = nullptr,
};

process::Process* g_console_owner = nullptr;
size_t g_console_refcount = 0;

void close_console(DescriptorEntry&) {
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
    if (proc.vty_id != 0) {
        alloc.type = kTypeConsole;
        alloc.flags = static_cast<uint64_t>(Flag::Writable);
        alloc.extended_flags = 0;
        alloc.has_extended_flags = false;
        alloc.object = kconsole;
        alloc.close = nullptr;
        alloc.name = "console";
        alloc.ops = &kConsoleOps;
        return true;
    }
    if (kconsole == nullptr) {
        return false;
    }
    if (g_console_owner != nullptr && g_console_owner != &proc) {
        return false;
    }
    g_console_owner = &proc;
    ++g_console_refcount;
    alloc.type = kTypeConsole;
    alloc.flags = static_cast<uint64_t>(Flag::Writable);
    alloc.extended_flags = 0;
    alloc.has_extended_flags = false;
    alloc.object = kconsole;
    alloc.close = close_console;
    alloc.name = "console";
    alloc.ops = &kConsoleOps;
    return true;
}

}  // namespace console_descriptor

bool register_console_descriptor() {
    return register_type(kTypeConsole, console_descriptor::open_console, &console_descriptor::kConsoleOps);
}

bool transfer_console_owner(process::Process& from, process::Process& to) {
    if (console_descriptor::g_console_owner != &from) {
        return false;
    }
    console_descriptor::g_console_owner = &to;
    console_descriptor::g_console_refcount = 0;
    return true;
}

void restore_console_owner(process::Process& proc) {
    console_descriptor::g_console_owner = &proc;
    console_descriptor::g_console_refcount = 1;
}

bool console_is_owner(const process::Process& proc) {
    return console_descriptor::g_console_owner == &proc;
}

}  // namespace descriptor
