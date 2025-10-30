#include "drivers/storage/ide_provider.hpp"

#include "drivers/log/logging.hpp"
#include "drivers/storage/ide.hpp"
#include "drivers/fs/block_device.hpp"
#include "drivers/fs/mount_manager.hpp"

namespace {

struct PartitionInfo {
    uint8_t type;
    uint8_t ordinal;
    uint32_t start_lba;
    uint32_t sector_count;
};

struct IdePartitionContext {
    IdeDeviceId device;
    uint32_t lba_base;
};

constexpr size_t kMaxPartitionsPerDevice = 4;
constexpr size_t kMaxDevices =
    kMaxPartitionsPerDevice * 4;  // four IDE device slots
constexpr size_t kMaxNameLen = 16;

constexpr struct {
    IdeDeviceId device;
    const char* base_name;
} kDeviceNames[] = {
    {IdeDeviceId::PrimaryMaster, "IDE_PM"},
    {IdeDeviceId::PrimarySlave, "IDE_PS"},
    {IdeDeviceId::SecondaryMaster, "IDE_SM"},
    {IdeDeviceId::SecondarySlave, "IDE_SS"},
};

alignas(512) uint8_t g_partition_buffer[512];
IdePartitionContext g_partition_contexts[kMaxDevices];
char g_name_storage[kMaxDevices][kMaxNameLen];

fs::BlockIoStatus ide_partition_read(void* context, uint32_t lba,
                                     uint8_t count, void* buffer) {
    auto* ctx = static_cast<IdePartitionContext*>(context);
    uint32_t absolute_lba = ctx->lba_base + lba;
    IdeStatus status =
        ide_read_sectors(ctx->device, absolute_lba, count, buffer);
    switch (status) {
        case IdeStatus::Ok:
            return fs::BlockIoStatus::Ok;
        case IdeStatus::Busy:
            return fs::BlockIoStatus::Busy;
        case IdeStatus::NoDevice:
            return fs::BlockIoStatus::NoDevice;
        default:
            return fs::BlockIoStatus::IoError;
    }
}

fs::BlockIoStatus ide_partition_write(void* context, uint32_t lba,
                                      uint8_t count, const void* buffer) {
    auto* ctx = static_cast<IdePartitionContext*>(context);
    uint32_t absolute_lba = ctx->lba_base + lba;
    IdeStatus status =
        ide_write_sectors(ctx->device, absolute_lba, count, buffer);
    switch (status) {
        case IdeStatus::Ok:
            return fs::BlockIoStatus::Ok;
        case IdeStatus::Busy:
            return fs::BlockIoStatus::Busy;
        case IdeStatus::NoDevice:
            return fs::BlockIoStatus::NoDevice;
        default:
            return fs::BlockIoStatus::IoError;
    }
}

uint32_t read_u32_le(const uint8_t* data) {
    return static_cast<uint32_t>(data[0]) |
           (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) |
           (static_cast<uint32_t>(data[3]) << 24);
}

bool is_fat32_partition(uint8_t type) {
    return type == 0x0B || type == 0x0C || type == 0x1B || type == 0x1C;
}

size_t copy_string(char* dest, size_t dest_size, const char* src) {
    if (dest == nullptr || dest_size == 0) {
        return 0;
    }
    size_t idx = 0;
    while (idx + 1 < dest_size && src[idx] != '\0') {
        dest[idx] = src[idx];
        ++idx;
    }
    dest[idx] = '\0';
    return idx;
}

bool append_suffix(char* buffer, size_t buffer_size, uint32_t value) {
    size_t len = 0;
    while (len < buffer_size && buffer[len] != '\0') {
        ++len;
    }
    if (len + 2 >= buffer_size) {
        return false;
    }
    buffer[len++] = '_';

    char digits[10];
    size_t digit_count = 0;
    do {
        digits[digit_count++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    } while (value > 0 && digit_count < sizeof(digits));

    if (len + digit_count >= buffer_size) {
        return false;
    }

    for (size_t i = 0; i < digit_count; ++i) {
        buffer[len + i] = digits[digit_count - 1 - i];
    }
    buffer[len + digit_count] = '\0';
    return true;
}

size_t scan_partitions(IdeDeviceId device, PartitionInfo* partitions,
                       size_t max_partitions) {
    if (partitions == nullptr || max_partitions == 0) {
        return 0;
    }

    IdeStatus status = ide_read_sectors(device, 0, 1, g_partition_buffer);
    if (status != IdeStatus::Ok) {
        log_message(LogLevel::Warn,
                    "IDE %s: failed to read partition table (status %d)",
                    ide_device_name(device), static_cast<int>(status));
        return 0;
    }

    if (g_partition_buffer[510] != 0x55 ||
        g_partition_buffer[511] != 0xAA) {
        return 0;
    }

    size_t count = 0;
    for (size_t entry = 0; entry < 4; ++entry) {
        const uint8_t* record = g_partition_buffer + 446 + (entry * 16);
        uint8_t type = record[4];
        uint32_t start_lba = read_u32_le(record + 8);
        uint32_t sectors = read_u32_le(record + 12);

        if (type == 0 || sectors == 0) {
            continue;
        }
        if (!is_fat32_partition(type)) {
            log_message(LogLevel::Info,
                        "IDE %s: partition %u type %02x unsupported",
                        ide_device_name(device), (unsigned int)entry, type);
            continue;
        }
        if (count >= max_partitions) {
            break;
        }

        partitions[count++] = {type, static_cast<uint8_t>(entry), start_lba,
                               sectors};
    }
    return count;
}

size_t enumerate_ide_devices(fs::BlockDevice* out_devices,
                             size_t max_devices) {
    if (out_devices == nullptr || max_devices == 0) {
        return 0;
    }

    size_t device_count = 0;

    for (const auto& cfg : kDeviceNames) {
        if (device_count >= max_devices) {
            break;
        }

        if (!ide_init(cfg.device)) {
            log_message(LogLevel::Info, "IDE %s: no device present",
                        ide_device_name(cfg.device));
            continue;
        }

        const IdeIdentifyInfo& identify = ide_identify(cfg.device);
        if (!identify.present) {
            continue;
        }

        PartitionInfo partitions[kMaxPartitionsPerDevice]{};
        size_t partition_count =
            scan_partitions(cfg.device, partitions, kMaxPartitionsPerDevice);

        bool use_whole_disk = false;
        if (partition_count == 0) {
            log_message(LogLevel::Info,
                        "IDE %s: no FAT32 partitions detected, using whole disk",
                        ide_device_name(cfg.device));
            partitions[0] = {0xFF, 0, 0, identify.sector_count};
            partition_count = 1;
            use_whole_disk = true;
        }

        for (size_t index = 0; index < partition_count; ++index) {
            if (device_count >= max_devices) {
                log_message(LogLevel::Warn,
                            "IDE provider: device list exhausted");
                break;
            }

            uint32_t partition_index =
                use_whole_disk ? 0 : partitions[index].ordinal;
            uint32_t base_lba =
                use_whole_disk ? 0 : partitions[index].start_lba;
            uint32_t sector_count =
                use_whole_disk ? identify.sector_count
                                : partitions[index].sector_count;

            IdePartitionContext& context = g_partition_contexts[device_count];
            context.device = cfg.device;
            context.lba_base = base_lba;

            char* name_buffer = g_name_storage[device_count];
            copy_string(name_buffer, kMaxNameLen, cfg.base_name);
            if (!append_suffix(name_buffer, kMaxNameLen, partition_index)) {
                log_message(LogLevel::Warn,
                            "IDE provider: mount name overflow for %s part %u",
                            cfg.base_name, (unsigned int)partition_index);
                continue;
            }

            fs::BlockDevice& device = out_devices[device_count];
            device.name = name_buffer;
            device.sector_size = 512;
            device.sector_count = sector_count;
            device.read = ide_partition_read;
            device.write = ide_partition_write;
            device.context = &context;

            ++device_count;
        }
    }

    return device_count;
}

}  // namespace

namespace fs {

void register_ide_block_device_provider() {
    static bool registered = false;
    if (registered) {
        return;
    }
    registered = true;
    register_block_device_provider(enumerate_ide_devices);
}

}  // namespace fs
