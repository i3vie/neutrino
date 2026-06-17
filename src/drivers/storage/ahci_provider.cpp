#include "drivers/storage/ahci_provider.hpp"

#include "drivers/fs/block_device.hpp"
#include "drivers/fs/mount_manager.hpp"
#include "drivers/log/logging.hpp"
#include "drivers/storage/ahci.hpp"

namespace {

struct PartitionInfo {
    uint8_t type;
    uint8_t ordinal;
    uint32_t start_lba;
    uint32_t sector_count;
};

struct AhciPartitionContext {
    size_t device_index;
    uint64_t lba_base;
};

constexpr size_t kMaxAhciDevices = 32;
constexpr size_t kMaxPartitionsPerDevice = 4;
constexpr size_t kMaxExportedDevices = kMaxAhciDevices * kMaxPartitionsPerDevice;
constexpr size_t kMaxNameLen = 24;

alignas(512) uint8_t g_partition_buffer[512];
AhciPartitionContext g_partition_contexts[kMaxExportedDevices];
char g_name_storage[kMaxExportedDevices][kMaxNameLen];

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
    while (idx + 1 < dest_size && src != nullptr && src[idx] != '\0') {
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

fs::BlockIoStatus ahci_partition_read(void* context,
                                      uint32_t lba,
                                      uint8_t count,
                                      void* buffer) {
    auto* ctx = static_cast<AhciPartitionContext*>(context);
    ahci::Status status =
        ahci::read_sectors(ctx->device_index, ctx->lba_base + lba, count, buffer);
    switch (status) {
        case ahci::Status::Ok:
            return fs::BlockIoStatus::Ok;
        case ahci::Status::Busy:
            return fs::BlockIoStatus::Busy;
        case ahci::Status::NoDevice:
            return fs::BlockIoStatus::NoDevice;
        default:
            return fs::BlockIoStatus::IoError;
    }
}

fs::BlockIoStatus ahci_partition_write(void* context,
                                       uint32_t lba,
                                       uint8_t count,
                                       const void* buffer) {
    auto* ctx = static_cast<AhciPartitionContext*>(context);
    ahci::Status status = ahci::write_sectors(
        ctx->device_index, ctx->lba_base + lba, count, buffer);
    switch (status) {
        case ahci::Status::Ok:
            return fs::BlockIoStatus::Ok;
        case ahci::Status::Busy:
            return fs::BlockIoStatus::Busy;
        case ahci::Status::NoDevice:
            return fs::BlockIoStatus::NoDevice;
        default:
            return fs::BlockIoStatus::IoError;
    }
}

size_t scan_partitions(size_t device_index,
                       PartitionInfo* partitions,
                       size_t max_partitions) {
    if (partitions == nullptr || max_partitions == 0) {
        return 0;
    }

    ahci::Status status = ahci::read_sectors(device_index, 0, 1, g_partition_buffer);
    if (status != ahci::Status::Ok) {
        log_message(LogLevel::Warn,
                    "AHCI %s: failed to read partition table (status %d)",
                    ahci::device_name(device_index),
                    static_cast<int>(status));
        return 0;
    }

    if (g_partition_buffer[510] != 0x55 || g_partition_buffer[511] != 0xAA) {
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
                        "AHCI %s: partition %u type %02x unsupported",
                        ahci::device_name(device_index),
                        static_cast<unsigned int>(entry),
                        static_cast<unsigned int>(type));
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

size_t enumerate_ahci_devices(fs::BlockDevice* out_devices, size_t max_devices) {
    if (out_devices == nullptr || max_devices == 0) {
        return 0;
    }

    size_t exported_count = 0;
    size_t controller_device_count = ahci::device_count();

    for (size_t device_index = 0; device_index < controller_device_count;
         ++device_index) {
        if (exported_count >= max_devices) {
            break;
        }

        const ahci::IdentifyInfo& identify = ahci::identify(device_index);
        if (!identify.present) {
            continue;
        }

        PartitionInfo partitions[kMaxPartitionsPerDevice]{};
        size_t partition_count =
            scan_partitions(device_index, partitions, kMaxPartitionsPerDevice);

        bool use_whole_disk = false;
        if (partition_count == 0) {
            log_message(LogLevel::Info,
                        "AHCI %s: no recognized partitions detected, using whole disk",
                        ahci::device_name(device_index));
            uint64_t disk_sectors = identify.sector_count;
            if (disk_sectors > 0xFFFFFFFFull) {
                disk_sectors = 0xFFFFFFFFull;
            }
            partitions[0] = {0xFF, 0, 0, static_cast<uint32_t>(disk_sectors)};
            partition_count = 1;
            use_whole_disk = true;
        }

        for (size_t partition_index = 0; partition_index < partition_count;
             ++partition_index) {
            if (exported_count >= max_devices) {
                log_message(LogLevel::Warn,
                            "AHCI provider: device list exhausted");
                break;
            }

            uint32_t partition_ordinal =
                use_whole_disk ? 0 : partitions[partition_index].ordinal;
            uint64_t base_lba =
                use_whole_disk ? 0 : partitions[partition_index].start_lba;
            uint64_t sector_count =
                use_whole_disk ? identify.sector_count
                               : partitions[partition_index].sector_count;

            AhciPartitionContext& context = g_partition_contexts[exported_count];
            context.device_index = device_index;
            context.lba_base = base_lba;

            char* name_buffer = g_name_storage[exported_count];
            copy_string(name_buffer, kMaxNameLen, ahci::device_name(device_index));
            if (!append_suffix(name_buffer, kMaxNameLen, partition_ordinal)) {
                log_message(LogLevel::Warn,
                            "AHCI provider: mount name overflow for %s part %u",
                            ahci::device_name(device_index),
                            static_cast<unsigned int>(partition_ordinal));
                continue;
            }

            fs::BlockDevice& device = out_devices[exported_count];
            device.name = name_buffer;
            device.parent_name = ahci::device_name(device_index);
            device.sector_size = 512;
            device.sector_count = sector_count;
            device.start_lba = base_lba;
            device.partition_index = partition_ordinal;
            device.partition_type = partitions[partition_index].type;
            device.descriptor_handle = descriptor::kInvalidHandle;
            device.read = ahci_partition_read;
            device.write = ahci_partition_write;
            device.context = &context;

            ++exported_count;
        }
    }

    return exported_count;
}

}  // namespace

namespace fs {

void register_ahci_block_device_provider() {
    static bool registered = false;
    if (registered) {
        return;
    }
    registered = true;
    register_block_device_provider(enumerate_ahci_devices);
}

}  // namespace fs
