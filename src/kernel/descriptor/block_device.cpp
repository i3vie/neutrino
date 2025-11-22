#include "../descriptor.hpp"

#include "../../drivers/fs/block_device.hpp"
#include "../../drivers/log/logging.hpp"
#include "../string_util.hpp"

namespace descriptor {

namespace block_device_descriptor {

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

}  // namespace block_device_descriptor

bool register_block_device_descriptor() {
    return register_type(kTypeBlockDevice, block_device_descriptor::open_block_device, &block_device_descriptor::kBlockDeviceOps);
}

bool register_block_device(fs::BlockDevice& device, bool lock_for_kernel) {
    if (device.name == nullptr) {
        return false;
    }
    block_device_descriptor::BlockDeviceRecord* slot = block_device_descriptor::find_block_device_by_name(device.name);
    if (slot == nullptr) {
        for (auto& candidate : block_device_descriptor::g_block_devices) {
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
    string_util::copy(slot->name, block_device_descriptor::kMaxBlockDeviceNameLen, device.name);
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
    block_device_descriptor::clear_block_devices();
}

}  // namespace descriptor
