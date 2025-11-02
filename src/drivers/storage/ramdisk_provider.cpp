#include "drivers/storage/ramdisk_provider.hpp"

#include <stddef.h>
#include <stdint.h>

#include "drivers/fs/block_device.hpp"
#include "drivers/fs/mount_manager.hpp"
#include "drivers/limine/limine_requests.hpp"
#include "drivers/log/logging.hpp"
#include "lib/mem.hpp"

namespace {

constexpr uint32_t kSectorSize = 512;
constexpr size_t kMaxModules = 16;
constexpr size_t kMaxPartitionsPerModule = 4;
constexpr size_t kMaxDevices = kMaxModules * (kMaxPartitionsPerModule + 1);
constexpr size_t kMaxNameLen = 32;

struct PartitionInfo {
    uint8_t type;
    uint8_t ordinal;
    uint32_t start_lba;
    uint32_t sector_count;
};

struct RamdiskPartitionContext {
    const uint8_t* base;
    uint64_t sector_count;
};

RamdiskPartitionContext g_partition_contexts[kMaxDevices];
char g_name_storage[kMaxDevices][kMaxNameLen];

uint32_t read_u32_le(const uint8_t* data) {
    return static_cast<uint32_t>(data[0]) |
           (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) |
           (static_cast<uint32_t>(data[3]) << 24);
}

size_t discover_partitions(const uint8_t* base,
                           uint64_t sector_count,
                           PartitionInfo* out_partitions,
                           size_t max_partitions) {
    if (base == nullptr || sector_count == 0 || out_partitions == nullptr ||
        max_partitions == 0) {
        return 0;
    }

    if (sector_count == 0) {
        return 0;
    }

    const uint8_t* mbr = base;
    if (mbr[510] != 0x55 || mbr[511] != 0xAA) {
        return 0;
    }

    size_t count = 0;
    for (size_t entry = 0; entry < 4 && count < max_partitions; ++entry) {
        const uint8_t* record = mbr + 446 + (entry * 16);
        uint8_t type = record[4];
        uint32_t start_lba = read_u32_le(record + 8);
        uint32_t sectors = read_u32_le(record + 12);

        if (type == 0 || sectors == 0) {
            continue;
        }

        if (start_lba >= sector_count) {
            log_message(LogLevel::Warn,
                        "Ramdisk: partition %u start beyond module (%u >= %llu)",
                        static_cast<unsigned int>(entry),
                        static_cast<unsigned int>(start_lba),
                        static_cast<unsigned long long>(sector_count));
            continue;
        }

        uint64_t available = sector_count - start_lba;
        if (sectors > available) {
            sectors = static_cast<uint32_t>(available);
        }

        if (sectors == 0) {
            continue;
        }

        out_partitions[count++] = {type,
                                   static_cast<uint8_t>(entry),
                                   start_lba,
                                   sectors};
    }

    return count;
}

bool append_literal(char* buffer, size_t buffer_size,
                    size_t& index, const char* literal) {
    if (buffer == nullptr || literal == nullptr) {
        return false;
    }

    for (size_t i = 0; literal[i] != '\0'; ++i) {
        if (index + 1 >= buffer_size) {
            return false;
        }
        buffer[index++] = literal[i];
    }
    return true;
}

bool append_decimal(char* buffer, size_t buffer_size,
                    size_t& index, size_t value) {
    if (buffer == nullptr) {
        return false;
    }

    char digits[20];
    size_t digit_count = 0;
    do {
        digits[digit_count++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    } while (value != 0 && digit_count < sizeof(digits));

    if (index + digit_count >= buffer_size) {
        return false;
    }

    for (size_t i = 0; i < digit_count; ++i) {
        buffer[index++] = digits[digit_count - 1 - i];
    }
    return true;
}

bool format_memdisk_name(char* buffer,
                         size_t buffer_size,
                         size_t module_index,
                         size_t partition_index) {
    if (buffer == nullptr || buffer_size == 0) {
        return false;
    }

    size_t index = 0;
    if (!append_literal(buffer, buffer_size, index, "MEMDISK_")) {
        return false;
    }
    if (!append_decimal(buffer, buffer_size, index, module_index)) {
        return false;
    }
    if (index + 1 >= buffer_size) {
        return false;
    }
    buffer[index++] = '_';
    if (!append_decimal(buffer, buffer_size, index, partition_index)) {
        return false;
    }
    if (index >= buffer_size) {
        return false;
    }
    buffer[index] = '\0';
    return true;
}

fs::BlockIoStatus ramdisk_read(void* context,
                               uint32_t lba,
                               uint8_t sector_count,
                               void* buffer) {
    if (context == nullptr || buffer == nullptr || sector_count == 0) {
        return fs::BlockIoStatus::NoDevice;
    }

    auto* ctx = static_cast<RamdiskPartitionContext*>(context);
    uint64_t requested = static_cast<uint64_t>(sector_count);
    if (requested == 0) {
        return fs::BlockIoStatus::Ok;
    }

    if (static_cast<uint64_t>(lba) >= ctx->sector_count) {
        return fs::BlockIoStatus::IoError;
    }

    uint64_t available = ctx->sector_count - static_cast<uint64_t>(lba);
    if (requested > available) {
        return fs::BlockIoStatus::IoError;
    }

    uint64_t offset =
        static_cast<uint64_t>(lba) * static_cast<uint64_t>(kSectorSize);
    uint64_t byte_count =
        requested * static_cast<uint64_t>(kSectorSize);
    memcpy(buffer, ctx->base + offset, static_cast<size_t>(byte_count));
    return fs::BlockIoStatus::Ok;
}

fs::BlockIoStatus ramdisk_write(void* context,
                                uint32_t,
                                uint8_t,
                                const void*) {
    if (context == nullptr) {
        return fs::BlockIoStatus::NoDevice;
    }
    return fs::BlockIoStatus::IoError;
}

const uint8_t* module_data_pointer(uintptr_t module_address) {
    if (module_address == 0) {
        return nullptr;
    }

    uint64_t hhdm_offset = 0;
    if (hhdm_request.response != nullptr) {
        hhdm_offset = hhdm_request.response->offset;
    }

    if (hhdm_offset != 0 && module_address < hhdm_offset) {
        module_address += hhdm_offset;
    }

    return reinterpret_cast<const uint8_t*>(module_address);
}

const char* module_label(const volatile struct limine_file* file) {
    if (file == nullptr) {
        return "(null)";
    }

    if (file->path != nullptr) {
        const char* path = reinterpret_cast<const char*>(file->path);
        if (path[0] != '\0') {
            return path;
        }
    }

#if LIMINE_API_REVISION >= 3
    if (file->string != nullptr) {
        const char* str = reinterpret_cast<const char*>(file->string);
        if (str[0] != '\0') {
            return str;
        }
    }
#else
    if (file->cmdline != nullptr) {
        const char* cmd = reinterpret_cast<const char*>(file->cmdline);
        if (cmd[0] != '\0') {
            return cmd;
        }
    }
#endif

    return "(unnamed module)";
}

size_t enumerate_ramdisks(fs::BlockDevice* out_devices, size_t max_devices) {
    if (out_devices == nullptr || max_devices == 0) {
        return 0;
    }

    const volatile struct limine_module_response* response =
        module_request.response;
    if (response == nullptr || response->module_count == 0 ||
        response->modules == nullptr) {
        return 0;
    }

    size_t device_count = 0;
    uint64_t module_count = response->module_count;

    for (uint64_t module_index = 0; module_index < module_count; ++module_index) {
        if (device_count >= max_devices) {
            break;
        }

        volatile struct limine_file* file = response->modules[module_index];
        if (file == nullptr || file->address == nullptr || file->size == 0) {
            continue;
        }

        uintptr_t module_address = reinterpret_cast<uintptr_t>(file->address);
        const uint8_t* module_base = module_data_pointer(module_address);
        if (module_base == nullptr) {
            log_message(LogLevel::Warn,
                        "Ramdisk: module %llu (%s) not accessible",
                        static_cast<unsigned long long>(module_index),
                        module_label(file));
            continue;
        }

        uint64_t total_bytes = file->size;
        if (total_bytes < kSectorSize) {
            log_message(LogLevel::Warn,
                        "Ramdisk: module %s smaller than one sector",
                        module_label(file));
            continue;
        }

        uint64_t total_sectors = total_bytes / kSectorSize;
        if (total_sectors == 0) {
            continue;
        }

        PartitionInfo partitions[kMaxPartitionsPerModule];
        size_t partition_count = discover_partitions(module_base,
                                                     total_sectors,
                                                     partitions,
                                                     kMaxPartitionsPerModule);
        bool use_entire_disk = partition_count == 0;
        if (use_entire_disk) {
            partitions[0] = {0xFF, 0, 0, static_cast<uint32_t>(total_sectors)};
            partition_count = 1;
        }

        for (size_t part_index = 0; part_index < partition_count; ++part_index) {
            if (device_count >= max_devices) {
                log_message(LogLevel::Warn,
                            "Ramdisk: device enumeration capacity reached");
                return device_count;
            }
            if (device_count >= kMaxDevices) {
                log_message(LogLevel::Warn,
                            "Ramdisk: internal device table exhausted");
                return device_count;
            }

            const PartitionInfo& part = partitions[part_index];

            uint64_t start_sector = part.start_lba;
            uint64_t sectors = part.sector_count;
            if (sectors == 0) {
                continue;
            }

            const uint8_t* partition_base =
                module_base + static_cast<size_t>(start_sector * kSectorSize);
            RamdiskPartitionContext& context =
                g_partition_contexts[device_count];
            context.base = partition_base;
            context.sector_count = sectors;

            char* name_buffer = g_name_storage[device_count];
            size_t logical_partition =
                use_entire_disk ? 0 : part.ordinal;
            if (!format_memdisk_name(name_buffer,
                                     kMaxNameLen,
                                     static_cast<size_t>(module_index),
                                     logical_partition)) {
                log_message(LogLevel::Warn,
                            "Ramdisk: failed to format name for module %llu part %zu",
                            static_cast<unsigned long long>(module_index),
                            logical_partition);
                continue;
            }

            fs::BlockDevice& device = out_devices[device_count];
            device.name = name_buffer;
            device.sector_size = kSectorSize;
            device.sector_count = sectors;
            device.read = ramdisk_read;
            device.write = ramdisk_write;
            device.context = &context;

            log_message(LogLevel::Info,
                        "Ramdisk: registered %s (%s, %llu sectors)",
                        device.name,
                        module_label(file),
                        static_cast<unsigned long long>(sectors));

            ++device_count;
        }
    }

    return device_count;
}

}  // namespace

namespace fs {

void register_ramdisk_block_device_provider() {
    static bool registered = false;
    if (registered) {
        return;
    }
    registered = true;
    register_block_device_provider(enumerate_ramdisks);
}

}  // namespace fs
