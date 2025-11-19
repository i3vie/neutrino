#include "descriptor.hpp"

#include "../drivers/console/console.hpp"
#include "../drivers/fs/block_device.hpp"
#include "../drivers/input/keyboard.hpp"
#include "../drivers/log/logging.hpp"
#include "../drivers/serial/serial.hpp"
#include "process.hpp"
#include "string_util.hpp"

namespace descriptor {

namespace {

int64_t console_read(process::Process&,
                     DescriptorEntry&,
                     uint64_t,
                     uint64_t,
                     uint64_t) {
    return -1;
}

int64_t console_write(process::Process&,
                      DescriptorEntry& entry,
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

int64_t serial_read(process::Process&,
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
    size_t read_count = serial::read(buffer, to_read);
    return static_cast<int64_t>(read_count);
}

int64_t serial_write(process::Process&,
                     DescriptorEntry&,
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
    .get_property = nullptr,
    .set_property = nullptr,
};

bool open_serial(process::Process&,
                 uint64_t,
                 uint64_t,
                 uint64_t,
                 Allocation& alloc) {
    serial::init();
    alloc.type = kTypeSerial;
    alloc.flags = static_cast<uint64_t>(Flag::Readable) |
                  static_cast<uint64_t>(Flag::Writable);
    alloc.extended_flags = 0;
    alloc.has_extended_flags = false;
    alloc.object = nullptr;
    alloc.close = nullptr;
    alloc.name = "serial";
    alloc.ops = &kSerialOps;
    return true;
}

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

struct FramebufferDescriptor {
    Framebuffer* fb;
    uint64_t physical_base;
    bool locked;
};

FramebufferDescriptor g_framebuffer_descriptor{nullptr, 0, false};
Framebuffer g_framebuffer_storage{};

int64_t framebuffer_read(process::Process&,
                         DescriptorEntry& entry,
                         uint64_t user_address,
                         uint64_t length,
                         uint64_t offset) {
    auto* holder = static_cast<FramebufferDescriptor*>(entry.object);
    if (holder == nullptr || holder->fb == nullptr) {
        return -1;
    }
    Framebuffer* fb = holder->fb;
    size_t frame_bytes = fb->pitch * fb->height;
    if (offset > frame_bytes) {
        return -1;
    }
    if (length == 0) {
        return 0;
    }
    if (offset + length > frame_bytes) {
        return -1;
    }
    auto* src = reinterpret_cast<uint8_t*>(fb->base);
    auto* dest = reinterpret_cast<uint8_t*>(user_address);
    if (src == nullptr || dest == nullptr) {
        return -1;
    }
    for (uint64_t i = 0; i < length; ++i) {
        dest[i] = src[offset + i];
    }
    return static_cast<int64_t>(length);
}

int64_t framebuffer_write(process::Process&,
                          DescriptorEntry& entry,
                          uint64_t user_address,
                          uint64_t length,
                          uint64_t offset) {
    auto* holder = static_cast<FramebufferDescriptor*>(entry.object);
    if (holder == nullptr || holder->fb == nullptr) {
        return -1;
    }
    Framebuffer* fb = holder->fb;
    size_t frame_bytes = fb->pitch * fb->height;
    if (offset > frame_bytes) {
        return -1;
    }
    if (length == 0) {
        return 0;
    }
    if (offset + length > frame_bytes) {
        return -1;
    }
    auto* dest = reinterpret_cast<uint8_t*>(fb->base);
    auto* src = reinterpret_cast<const uint8_t*>(user_address);
    if (dest == nullptr || src == nullptr) {
        return -1;
    }
    for (uint64_t i = 0; i < length; ++i) {
        dest[offset + i] = src[i];
    }
    return static_cast<int64_t>(length);
}

int framebuffer_get_property(DescriptorEntry& entry,
                             uint32_t property,
                             void* out,
                             size_t size) {
    auto* holder = static_cast<FramebufferDescriptor*>(entry.object);
    if (holder == nullptr || holder->fb == nullptr) {
        return -1;
    }
    Framebuffer* fb = holder->fb;
    if (property ==
        static_cast<uint32_t>(descriptor_defs::Property::FramebufferInfo)) {
        if (out == nullptr || size < sizeof(descriptor_defs::FramebufferInfo)) {
            return -1;
        }
        auto* info = reinterpret_cast<descriptor_defs::FramebufferInfo*>(out);
        info->physical_base = g_framebuffer_descriptor.physical_base;
        info->virtual_base = reinterpret_cast<uint64_t>(fb->base);
        info->width = static_cast<uint32_t>(fb->width);
        info->height = static_cast<uint32_t>(fb->height);
        info->pitch = static_cast<uint32_t>(fb->pitch);
        info->bpp = fb->bpp;
        info->memory_model = fb->memory_model;
        info->reserved = 0;
        info->red_mask_size = fb->red_mask_size;
        info->red_mask_shift = fb->red_mask_shift;
        info->green_mask_size = fb->green_mask_size;
        info->green_mask_shift = fb->green_mask_shift;
        info->blue_mask_size = fb->blue_mask_size;
        info->blue_mask_shift = fb->blue_mask_shift;
        return 0;
    }
    return -1;
}

const Ops kFramebufferOps{
    .read = framebuffer_read,
    .write = framebuffer_write,
    .get_property = framebuffer_get_property,
    .set_property = nullptr,
};

bool open_framebuffer(process::Process& proc,
                      uint64_t index,
                      uint64_t,
                      uint64_t,
                      Allocation& alloc) {
    if (index != 0) {
        return false;
    }
    if (g_framebuffer_descriptor.fb == nullptr) {
        return false;
    }
    bool is_kernel = is_kernel_process(proc);
    if (g_framebuffer_descriptor.locked && !is_kernel) {
        return false;
    }
    alloc.type = kTypeFramebuffer;
    alloc.flags = static_cast<uint64_t>(Flag::Readable) |
                  static_cast<uint64_t>(Flag::Writable) |
                  static_cast<uint64_t>(Flag::Mappable) |
                  static_cast<uint64_t>(Flag::Device);
    alloc.extended_flags = 0;
    alloc.has_extended_flags = false;
    alloc.object = &g_framebuffer_descriptor;
    alloc.subsystem_data = nullptr;
    alloc.close = nullptr;
    alloc.name = "framebuffer";
    alloc.ops = &kFramebufferOps;
    if (is_kernel) {
        g_framebuffer_descriptor.locked = true;
    }
    return true;
}

constexpr size_t kMaxBlockDescriptors = 32;
constexpr size_t kMaxBlockDeviceNameLen = 32;

struct BlockDeviceRecord {
    fs::BlockDevice device;
    uint32_t handle;
    bool locked;
    bool in_use;
    char name[kMaxBlockDeviceNameLen];
};

BlockDeviceRecord g_block_devices[kMaxBlockDescriptors];

BlockDeviceRecord* find_block_device_by_name(const char* name) {
    if (name == nullptr) {
        return nullptr;
    }
    for (auto& record : g_block_devices) {
        if (!record.in_use) {
            continue;
        }
        if (string_util::equals(record.name, name)) {
            return &record;
        }
    }
    return nullptr;
}

BlockDeviceRecord* find_block_device_by_index(uint64_t index) {
    size_t count = 0;
    for (auto& record : g_block_devices) {
        if (!record.in_use) {
            continue;
        }
        if (count == index) {
            return &record;
        }
        ++count;
    }
    return nullptr;
}

int64_t block_device_read(process::Process& proc,
                          DescriptorEntry& entry,
                          uint64_t user_address,
                          uint64_t length,
                          uint64_t offset) {
    auto* record = static_cast<BlockDeviceRecord*>(entry.object);
    if (record == nullptr || !record->in_use) {
        return -1;
    }
    if (record->locked && !is_kernel_process(proc)) {
        return -1;
    }
    if (length == 0) {
        return 0;
    }
    uint64_t sector_size = record->device.sector_size;
    if (sector_size == 0) {
        return -1;
    }
    if ((offset % sector_size) != 0) {
        return -1;
    }
    if ((length % sector_size) != 0) {
        return -1;
    }
    uint64_t sector_count = length / sector_size;
    if (sector_count == 0 || sector_count > 0xFF) {
        return -1;
    }
    uint64_t lba = offset / sector_size;
    if (lba >= record->device.sector_count) {
        return -1;
    }
    if (lba + sector_count > record->device.sector_count) {
        return -1;
    }
    void* buffer = reinterpret_cast<void*>(user_address);
    if (buffer == nullptr) {
        return -1;
    }
    auto read_fn = record->device.read;
    if (read_fn == nullptr) {
        return -1;
    }
    fs::BlockIoStatus status = read_fn(record->device.context,
                                       static_cast<uint32_t>(lba),
                                       static_cast<uint8_t>(sector_count),
                                       buffer);
    if (status != fs::BlockIoStatus::Ok) {
        return -1;
    }
    return static_cast<int64_t>(length);
}

int64_t block_device_write(process::Process& proc,
                           DescriptorEntry& entry,
                           uint64_t user_address,
                           uint64_t length,
                           uint64_t offset) {
    auto* record = static_cast<BlockDeviceRecord*>(entry.object);
    if (record == nullptr || !record->in_use) {
        return -1;
    }
    if (record->locked && !is_kernel_process(proc)) {
        return -1;
    }
    if (length == 0) {
        return 0;
    }
    uint64_t sector_size = record->device.sector_size;
    if (sector_size == 0) {
        return -1;
    }
    if ((offset % sector_size) != 0) {
        return -1;
    }
    if ((length % sector_size) != 0) {
        return -1;
    }
    uint64_t sector_count = length / sector_size;
    if (sector_count == 0 || sector_count > 0xFF) {
        return -1;
    }
    uint64_t lba = offset / sector_size;
    if (lba >= record->device.sector_count) {
        return -1;
    }
    if (lba + sector_count > record->device.sector_count) {
        return -1;
    }
    const void* buffer = reinterpret_cast<const void*>(user_address);
    if (buffer == nullptr) {
        return -1;
    }
    auto write_fn = record->device.write;
    if (write_fn == nullptr) {
        return -1;
    }
    fs::BlockIoStatus status =
        write_fn(record->device.context,
                 static_cast<uint32_t>(lba),
                 static_cast<uint8_t>(sector_count),
                 buffer);
    if (status != fs::BlockIoStatus::Ok) {
        return -1;
    }
    return static_cast<int64_t>(length);
}

int block_device_get_property(DescriptorEntry& entry,
                              uint32_t property,
                              void* out,
                              size_t size) {
    auto* record = static_cast<BlockDeviceRecord*>(entry.object);
    if (record == nullptr || !record->in_use) {
        return -1;
    }
    if (property ==
        static_cast<uint32_t>(descriptor_defs::Property::BlockGeometry)) {
        if (out == nullptr || size < sizeof(descriptor_defs::BlockGeometry)) {
            return -1;
        }
        auto* geom = reinterpret_cast<descriptor_defs::BlockGeometry*>(out);
        geom->sector_size = record->device.sector_size;
        geom->sector_count = record->device.sector_count;
        return 0;
    }
    return -1;
}

const Ops kBlockDeviceOps{
    .read = block_device_read,
    .write = block_device_write,
    .get_property = block_device_get_property,
    .set_property = nullptr,
};

bool open_block_device(process::Process& proc,
                       uint64_t name_ptr,
                       uint64_t index,
                       uint64_t,
                       Allocation& alloc) {
    BlockDeviceRecord* record = nullptr;
    if (name_ptr != 0) {
        const char* name = reinterpret_cast<const char*>(name_ptr);
        record = find_block_device_by_name(name);
    } else {
        record = find_block_device_by_index(index);
    }
    if (record == nullptr || !record->in_use) {
        return false;
    }
    if (record->locked && !is_kernel_process(proc)) {
        return false;
    }
    alloc.type = kTypeBlockDevice;
    uint64_t flags = static_cast<uint64_t>(Flag::Seekable) |
                     static_cast<uint64_t>(Flag::Device) |
                     static_cast<uint64_t>(Flag::Block);
    if (record->device.read != nullptr) {
        flags |= static_cast<uint64_t>(Flag::Readable);
    }
    if (record->device.write != nullptr) {
        flags |= static_cast<uint64_t>(Flag::Writable);
    }
    alloc.flags = flags;
    alloc.extended_flags = 0;
    alloc.has_extended_flags = false;
    alloc.object = record;
    alloc.subsystem_data = nullptr;
    alloc.close = nullptr;
    alloc.name = record->name;
    alloc.ops = &kBlockDeviceOps;
    return true;
}

void clear_block_devices() {
    for (auto& record : g_block_devices) {
        if (record.handle != kInvalidHandle) {
            close_kernel(record.handle);
        }
        record.in_use = false;
        record.name[0] = '\0';
        record.device = {};
        record.device.descriptor_handle = kInvalidHandle;
        record.handle = kInvalidHandle;
        record.locked = false;
    }
}
}  // namespace

bool transfer_console_owner(process::Process& from, process::Process& to) {
    if (g_console_owner != &from) {
        return false;
    }
    g_console_owner = &to;
    g_console_refcount = 0;
    return true;
}

void restore_console_owner(process::Process& proc) {
    g_console_owner = &proc;
    g_console_refcount = 1;
}

void register_builtin_types() {
    clear_block_devices();
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
    if (!register_type(kTypeFramebuffer, open_framebuffer, &kFramebufferOps)) {
        log_message(LogLevel::Warn,
                    "Descriptor: failed to register framebuffer descriptor type");
    }
    if (!register_type(kTypeBlockDevice, open_block_device, &kBlockDeviceOps)) {
        log_message(LogLevel::Warn,
                    "Descriptor: failed to register block device descriptor type");
    }
}

void register_framebuffer_device(Framebuffer& framebuffer,
                                 uint64_t physical_base) {
    g_framebuffer_storage = framebuffer;
    g_framebuffer_descriptor.fb = &g_framebuffer_storage;
    g_framebuffer_descriptor.physical_base = physical_base;
    g_framebuffer_descriptor.locked = false;
}

bool register_block_device(fs::BlockDevice& device, bool lock_for_kernel) {
    if (device.name == nullptr) {
        return false;
    }
    BlockDeviceRecord* slot = find_block_device_by_name(device.name);
    if (slot == nullptr) {
        for (auto& candidate : g_block_devices) {
            if (!candidate.in_use) {
                slot = &candidate;
                break;
            }
        }
    }
    if (slot == nullptr) {
        log_message(LogLevel::Warn,
                    "Descriptor: block device registry full, dropping %s",
                    device.name);
        return false;
    }
    slot->device = device;
    string_util::copy(slot->name, kMaxBlockDeviceNameLen, device.name);
    slot->device.name = slot->name;
    slot->locked = lock_for_kernel;
    slot->in_use = true;
    slot->handle = kInvalidHandle;
    slot->device.descriptor_handle = kInvalidHandle;
    device.name = slot->name;
    device.descriptor_handle = kInvalidHandle;

    if (lock_for_kernel) {
        slot->handle = open_kernel(kTypeBlockDevice,
                                   reinterpret_cast<uint64_t>(slot->name),
                                   0,
                                   0);
        if (slot->handle == kInvalidHandle) {
            log_message(LogLevel::Warn,
                        "Descriptor: failed to open block device descriptor for %s",
                        slot->name);
            slot->device = {};
            slot->in_use = false;
            slot->locked = false;
            slot->name[0] = '\0';
            device.descriptor_handle = kInvalidHandle;
            return false;
        }
        slot->device.descriptor_handle = slot->handle;
        device.descriptor_handle = slot->handle;
    }

    return true;
}

void reset_block_device_registry() {
    clear_block_devices();
}

}  // namespace descriptor
