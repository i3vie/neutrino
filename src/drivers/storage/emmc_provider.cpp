#include "drivers/storage/emmc_provider.hpp"

#include "drivers/fs/block_device.hpp"
#include "drivers/fs/mount_manager.hpp"
#include "drivers/log/logging.hpp"
#include "drivers/storage/emmc.hpp"

namespace {

constexpr size_t kMaxEntries = 32;  // raw devices + partitions
constexpr size_t kMaxRawDevices = 16;
constexpr size_t kNameLength = 16;
constexpr size_t kSectorSize = 512;

fs::BlockIoStatus emmc_block_read(void* context,
                                  uint32_t lba,
                                  uint8_t count,
                                  void* buffer);

#pragma pack(push, 1)
struct MbrPartitionEntry {
    uint8_t  status;
    uint8_t  chs_first[3];
    uint8_t  type;
    uint8_t  chs_last[3];
    uint32_t lba_first;
    uint32_t sectors;
};
#pragma pack(pop)

struct EmmcContext {
    size_t device_index;
    uint32_t lba_offset;  // added offset for partitions
};

EmmcContext g_contexts[kMaxEntries];
char g_name_storage[kMaxEntries][kNameLength];
constexpr const char kProviderName[] = "EMMC";

bool ensure_capacity(size_t index) {
    if (index < kMaxEntries) {
        return true;
    }
    log_message(LogLevel::Warn,
                "eMMC provider: out of context slots (max %zu)",
                static_cast<size_t>(kMaxEntries));
    return false;
}

bool append_number(char* buffer, size_t buffer_len, size_t& offset,
                   size_t value) {
    if (offset >= buffer_len) {
        return false;
    }
    char digits[20];
    size_t digit_count = 0;
    do {
        digits[digit_count++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    } while (value > 0 && digit_count < sizeof(digits));

    if (offset + digit_count >= buffer_len) {
        return false;
    }

    for (size_t i = 0; i < digit_count; ++i) {
        buffer[offset + i] = digits[digit_count - 1 - i];
    }
    offset += digit_count;
    buffer[offset] = '\0';
    return true;
}

bool format_emmc_name(char* buffer, size_t buffer_len, size_t device_index,
                      bool include_partition, size_t partition_index) {
    if (buffer == nullptr || buffer_len == 0) {
        return false;
    }

    size_t offset = 0;
    for (size_t i = 0; kProviderName[i] != '\0'; ++i) {
        if (offset + 1 >= buffer_len) {
            return false;
        }
        buffer[offset++] = kProviderName[i];
    }

    buffer[offset++] = '_';
    if (offset >= buffer_len) {
        return false;
    }
    if (!append_number(buffer, buffer_len, offset, device_index)) {
        return false;
    }

    if (include_partition) {
        if (offset + 1 >= buffer_len) {
            return false;
        }
        buffer[offset++] = '_';
        buffer[offset] = '\0';
        if (!append_number(buffer, buffer_len, offset, partition_index)) {
            return false;
        }
    }

    return true;
}

bool add_block_device(fs::BlockDevice* out_devices,
                      size_t max_devices,
                      size_t& total,
                      size_t context_index,
                      size_t device_index,
                      uint32_t lba_offset,
                      uint64_t sector_count,
                      const char* name_source) {
    if (total >= max_devices) {
        return false;
    }
    if (!ensure_capacity(context_index)) {
        return false;
    }

    EmmcContext& ctx = g_contexts[context_index];
    ctx.device_index = device_index;
    ctx.lba_offset = lba_offset;

    fs::BlockDevice& dev = out_devices[total];
    dev.name = name_source;
    dev.sector_size = kSectorSize;
    dev.sector_count = sector_count;
    dev.descriptor_handle = descriptor::kInvalidHandle;
    dev.read = emmc_block_read;
    dev.write = nullptr;
    dev.context = &ctx;
    ++total;
    return true;
}

enum class PartitionType {
    Unused,
    Primary,
    Extended,
};

PartitionType classify_partition(uint8_t type) {
    if (type == 0 || type == 0x7F) {
        return PartitionType::Unused;
    }
    if (type == 0x05 || type == 0x0F || type == 0x85) {
        return PartitionType::Extended;
    }
    return PartitionType::Primary;
}

bool add_partition_device(fs::BlockDevice* out_devices,
                          size_t max_devices,
                          size_t& total,
                          size_t device_index,
                          size_t partition_number,
                          uint32_t lba_start,
                          uint32_t sectors,
                          uint8_t type) {
    if (sectors == 0) {
        return false;
    }
    if (!ensure_capacity(total)) {
        return false;
    }

    char* part_name = g_name_storage[total];
    if (!format_emmc_name(part_name,
                          kNameLength,
                          device_index,
                          true,
                          partition_number)) {
        log_message(LogLevel::Warn,
                    "eMMC provider: failed to format partition name %zu:%zu",
                    device_index,
                    partition_number);
        return false;
    }

    if (!add_block_device(out_devices,
                          max_devices,
                          total,
                          total,
                          device_index,
                          lba_start,
                          static_cast<uint64_t>(sectors),
                          part_name)) {
        return false;
    }

    log_message(LogLevel::Info,
                "eMMC: partition %s type=%02x start=%u sectors=%u",
                part_name,
                static_cast<unsigned>(type),
                lba_start,
                sectors);
    return true;
}

void enumerate_extended_partitions(size_t device_index,
                                   uint32_t base_lba,
                                   fs::BlockDevice* out_devices,
                                   size_t max_devices,
                                   size_t& total,
                                   size_t& partition_number) {
    if (base_lba == 0) {
        return;
    }

    alignas(512) uint8_t sector[kSectorSize];
    uint32_t current_ebr = base_lba;

    while (total < max_devices) {
        if (emmc::read_blocks(device_index, current_ebr, 1, sector) !=
            emmc::Status::Ok) {
            log_message(LogLevel::Warn,
                        "eMMC: failed to read EBR at LBA %u on device %zu",
                        current_ebr,
                        device_index);
            break;
        }
        if (sector[510] != 0x55 || sector[511] != 0xAA) {
            log_message(LogLevel::Warn,
                        "eMMC: invalid EBR signature at LBA %u",
                        current_ebr);
            break;
        }

        auto* entries =
            reinterpret_cast<MbrPartitionEntry*>(sector + 446);
        const MbrPartitionEntry& logical = entries[0];
        const MbrPartitionEntry& next = entries[1];

        if (logical.type != 0 && logical.sectors != 0) {
            uint32_t logical_lba = current_ebr + logical.lba_first;
            if (add_partition_device(out_devices,
                                     max_devices,
                                     total,
                                     device_index,
                                     partition_number,
                                     logical_lba,
                                     logical.sectors,
                                     logical.type)) {
                ++partition_number;
            }
        }

        PartitionType next_type = classify_partition(next.type);
        if (next_type == PartitionType::Extended && next.lba_first != 0) {
            current_ebr = base_lba + next.lba_first;
        } else if (next_type == PartitionType::Primary &&
                   next.lba_first != 0) {
            // Some tools place the first logical partition in entry 1.
            current_ebr = base_lba + next.lba_first;
        } else {
            break;
        }
    }
}

fs::BlockIoStatus emmc_block_read(void* context,
                                  uint32_t lba,
                                  uint8_t count,
                                  void* buffer) {
    if (context == nullptr || buffer == nullptr || count == 0) {
        return fs::BlockIoStatus::NoDevice;
    }
    auto* ctx = static_cast<EmmcContext*>(context);
    emmc::Status status = emmc::read_blocks(ctx->device_index,
                                            ctx->lba_offset + lba,
                                            count,
                                            buffer);
    switch (status) {
        case emmc::Status::Ok:
            return fs::BlockIoStatus::Ok;
        case emmc::Status::Busy:
            return fs::BlockIoStatus::Busy;
        case emmc::Status::NoDevice:
            return fs::BlockIoStatus::NoDevice;
        default:
            return fs::BlockIoStatus::IoError;
    }
}

size_t enumerate_emmc_devices(fs::BlockDevice* out_devices,
                              size_t max_devices) {
    if (out_devices == nullptr || max_devices == 0) {
        return 0;
    }

    if (!emmc::init()) {
        return 0;
    }

    size_t total = 0;
    size_t device_count = emmc::device_count();
    if (device_count > kMaxRawDevices) {
        device_count = kMaxRawDevices;
    }

    for (size_t i = 0; i < device_count && total < max_devices; ++i) {
        if (!ensure_capacity(total)) {
            break;
        }

        char* raw_name = g_name_storage[total];
        if (!format_emmc_name(raw_name, kNameLength, i, false, 0)) {
            log_message(LogLevel::Warn,
                        "eMMC provider: failed to format name for device %zu",
                        i);
            continue;
        }

        uint64_t device_sectors = emmc::device_sector_count(i);
        if (!add_block_device(out_devices,
                              max_devices,
                              total,
                              total,
                              i,
                              0,
                              device_sectors,
                              raw_name)) {
            break;
        }

        log_message(LogLevel::Info,
                    "eMMC: device %s sectors=%llu",
                    raw_name,
                    static_cast<unsigned long long>(device_sectors));

        if (total >= max_devices) {
            break;
        }

        alignas(512) uint8_t sector[kSectorSize];
        if (emmc::read_blocks(i, 0, 1, sector) != emmc::Status::Ok) {
            log_message(LogLevel::Warn,
                        "eMMC: failed to read MBR for device %zu",
                        i);
            continue;
        }

        if (sector[510] != 0x55 || sector[511] != 0xAA) {
            log_message(LogLevel::Info,
                        "eMMC: device %zu has no valid MBR signature",
                        i);
            continue;
        }

        auto* entries = reinterpret_cast<MbrPartitionEntry*>(&sector[446]);
        size_t partition_number = 0;

        for (int p = 0; p < 4 && total < max_devices; ++p) {
            PartitionType type = classify_partition(entries[p].type);
            if (type == PartitionType::Unused ||
                entries[p].sectors == 0) {
                continue;
            }

            if (type == PartitionType::Extended) {
                enumerate_extended_partitions(i,
                                              entries[p].lba_first,
                                              out_devices,
                                              max_devices,
                                              total,
                                              partition_number);
                continue;
            }

            if (add_partition_device(out_devices,
                                     max_devices,
                                     total,
                                     i,
                                     partition_number,
                                     entries[p].lba_first,
                                     entries[p].sectors,
                                     entries[p].type)) {
                ++partition_number;
            }
        }
    }

    return total;
}

}  // namespace

namespace fs {

void register_emmc_block_device_provider() {
    register_block_device_provider(enumerate_emmc_devices);
}

}  // namespace fs
