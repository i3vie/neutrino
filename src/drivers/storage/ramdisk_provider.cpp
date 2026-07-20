#include "drivers/storage/ramdisk_provider.hpp"

#include <stddef.h>
#include <stdint.h>

#include "arch/x86_64/memory/paging.hpp"
#include "drivers/fs/block_device.hpp"
#include "drivers/fs/mount_manager.hpp"
#include "drivers/limine/limine_requests.hpp"
#include "drivers/log/logging.hpp"
#include "kernel/memory/physical_allocator.hpp"
#include "kernel/sync.hpp"
#include "lib/mem.hpp"

namespace {

constexpr uint32_t kSectorSize = 512;
constexpr size_t kMaxModules = 16;
constexpr size_t kMaxPartitionsPerModule = 4;
constexpr size_t kMaxDevices = kMaxModules * (kMaxPartitionsPerModule + 1);
constexpr size_t kMaxNameLen = 32;
constexpr size_t kPageSize = 4096;
constexpr size_t kSectorsPerOverlayPage = kPageSize / kSectorSize;
constexpr size_t kOverlayBucketCount = 256;
// The kernel allocator has a 64 MiB pool. Keep enough metadata to describe at
// most that many overlay pages; allocation will normally stop earlier as the
// rest of the kernel also uses that pool.
constexpr size_t kMaxOverlayPages = (64 * 1024 * 1024) / kPageSize;

struct PartitionInfo {
    uint8_t type;
    uint8_t ordinal;
    uint32_t start_lba;
    uint32_t sector_count;
};

struct RamdiskPartitionContext {
    const uint8_t* base;
    uint64_t sector_count;
    struct OverlayPage* overlay[kOverlayBucketCount];
    sync::SpinLock overlay_lock;
    bool allocation_failure_logged;
};

struct OverlayPage {
    OverlayPage* next;
    uint8_t* data;
    uint32_t first_lba;
};

RamdiskPartitionContext g_partition_contexts[kMaxDevices];
char g_name_storage[kMaxDevices][kMaxNameLen];
OverlayPage g_overlay_pages[kMaxOverlayPages];
size_t g_overlay_page_count = 0;

size_t overlay_bucket(uint32_t first_lba) {
    return (first_lba / kSectorsPerOverlayPage) % kOverlayBucketCount;
}

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

    auto* out = static_cast<uint8_t*>(buffer);
    uint32_t current_lba = lba;
    uint64_t remaining = requested;
    while (remaining != 0) {
        uint32_t first_lba =
            current_lba - (current_lba % kSectorsPerOverlayPage);
        size_t page_offset =
            static_cast<size_t>(current_lba - first_lba) * kSectorSize;
        size_t page_sectors = kSectorsPerOverlayPage -
                              static_cast<size_t>(current_lba - first_lba);
        if (page_sectors > remaining) {
            page_sectors = static_cast<size_t>(remaining);
        }
        size_t byte_count = page_sectors * kSectorSize;

        bool found = false;
        {
            sync::IrqLockGuard guard(ctx->overlay_lock);
            size_t bucket = overlay_bucket(first_lba);
            for (OverlayPage* page = ctx->overlay[bucket]; page != nullptr;
                 page = page->next) {
                if (page->first_lba == first_lba) {
                    memcpy(out, page->data + page_offset, byte_count);
                    found = true;
                    break;
                }
            }
        }
        if (!found) {
            uint64_t offset = static_cast<uint64_t>(current_lba) * kSectorSize;
            memcpy(out, ctx->base + offset, byte_count);
        }

        out += byte_count;
        current_lba += static_cast<uint32_t>(page_sectors);
        remaining -= page_sectors;
    }
    return fs::BlockIoStatus::Ok;
}

OverlayPage* find_overlay_page(RamdiskPartitionContext& context,
                               uint32_t first_lba) {
    size_t bucket = overlay_bucket(first_lba);
    for (OverlayPage* page = context.overlay[bucket]; page != nullptr;
         page = page->next) {
        if (page->first_lba == first_lba) {
            return page;
        }
    }
    return nullptr;
}

OverlayPage* create_overlay_page(RamdiskPartitionContext& context,
                                 uint32_t first_lba) {
    uint64_t physical_address = memory::alloc_kernel_page();
    if (physical_address == 0) {
        return nullptr;
    }

    auto* data = static_cast<uint8_t*>(paging_phys_to_virt(physical_address));
    uint64_t available = context.sector_count - first_lba;
    size_t sectors_to_copy = available < kSectorsPerOverlayPage
                                 ? static_cast<size_t>(available)
                                 : kSectorsPerOverlayPage;
    memcpy(data,
           context.base + static_cast<uint64_t>(first_lba) * kSectorSize,
           sectors_to_copy * kSectorSize);

    sync::IrqLockGuard guard(context.overlay_lock);
    OverlayPage* existing = find_overlay_page(context, first_lba);
    if (existing != nullptr) {
        memory::free_kernel_page(physical_address);
        return existing;
    }

    size_t node_index =
        __atomic_fetch_add(&g_overlay_page_count, size_t{1}, __ATOMIC_RELAXED);
    if (node_index >= kMaxOverlayPages) {
        memory::free_kernel_page(physical_address);
        return nullptr;
    }

    OverlayPage& page = g_overlay_pages[node_index];
    size_t bucket = overlay_bucket(first_lba);
    page.next = context.overlay[bucket];
    page.data = data;
    page.first_lba = first_lba;
    context.overlay[bucket] = &page;
    return &page;
}

fs::BlockIoStatus ramdisk_write(void* context,
                                uint32_t lba,
                                uint8_t sector_count,
                                const void* buffer) {
    if (context == nullptr || buffer == nullptr || sector_count == 0) {
        return fs::BlockIoStatus::NoDevice;
    }

    auto* ctx = static_cast<RamdiskPartitionContext*>(context);
    uint64_t requested = static_cast<uint64_t>(sector_count);
    if (static_cast<uint64_t>(lba) >= ctx->sector_count ||
        requested > ctx->sector_count - static_cast<uint64_t>(lba)) {
        return fs::BlockIoStatus::IoError;
    }

    const auto* in = static_cast<const uint8_t*>(buffer);
    uint32_t current_lba = lba;
    uint64_t remaining = requested;
    while (remaining != 0) {
        uint32_t first_lba =
            current_lba - (current_lba % kSectorsPerOverlayPage);
        size_t page_sector_offset = current_lba - first_lba;
        size_t page_sectors = kSectorsPerOverlayPage - page_sector_offset;
        if (page_sectors > remaining) {
            page_sectors = static_cast<size_t>(remaining);
        }

        OverlayPage* page = nullptr;
        {
            sync::IrqLockGuard guard(ctx->overlay_lock);
            page = find_overlay_page(*ctx, first_lba);
        }
        if (page == nullptr) {
            page = create_overlay_page(*ctx, first_lba);
            if (page == nullptr) {
                if (!__atomic_exchange_n(&ctx->allocation_failure_logged,
                                         true,
                                         __ATOMIC_RELAXED)) {
                    log_message(LogLevel::Error,
                                "Ramdisk: RAM write overlay allocation failed at LBA %u",
                                static_cast<unsigned int>(current_lba));
                }
                return fs::BlockIoStatus::IoError;
            }
        }

        size_t byte_offset = page_sector_offset * kSectorSize;
        size_t byte_count = page_sectors * kSectorSize;
        {
            sync::IrqLockGuard guard(ctx->overlay_lock);
            memcpy(page->data + byte_offset, in, byte_count);
        }
        in += byte_count;
        current_lba += static_cast<uint32_t>(page_sectors);
        remaining -= page_sectors;
    }
    return fs::BlockIoStatus::Ok;
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

const char* module_label(const PreservedLimineModule* module) {
    if (module == nullptr) {
        return "(null)";
    }

    if (module->path != nullptr && module->path[0] != '\0') {
        return module->path;
    }

    if (module->string != nullptr && module->string[0] != '\0') {
        return module->string;
    }

    return "(unnamed module)";
}

size_t enumerate_ramdisks(fs::BlockDevice* out_devices, size_t max_devices) {
    if (out_devices == nullptr || max_devices == 0) {
        return 0;
    }

    size_t module_count = preserved_limine_module_count();
    if (module_count == 0) {
        return 0;
    }

    size_t device_count = 0;

    for (size_t module_index = 0; module_index < module_count; ++module_index) {
        if (device_count >= max_devices) {
            break;
        }

        const PreservedLimineModule* module =
            preserved_limine_module(module_index);
        if (module == nullptr || module->address == nullptr || module->size == 0) {
            continue;
        }

        uintptr_t module_address = reinterpret_cast<uintptr_t>(module->address);
        const uint8_t* module_base = module_data_pointer(module_address);
        if (module_base == nullptr) {
            log_message(LogLevel::Warn,
                        "Ramdisk: module %zu (%s) not accessible",
                        module_index,
                        module_label(module));
            continue;
        }

        uint64_t total_bytes = module->size;
        if (total_bytes < kSectorSize) {
            log_message(LogLevel::Warn,
                        "Ramdisk: module %s smaller than one sector",
                        module_label(module));
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
                                     module_index,
                                     logical_partition)) {
                log_message(LogLevel::Warn,
                            "Ramdisk: failed to format name for module %zu part %zu",
                            module_index,
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
                        "Ramdisk: registered %s (%s, start=%llu sectors=%llu, RAM write overlay, bpb=%02x%02x sig=%02x%02x)",
                        device.name,
                        module_label(module),
                        static_cast<unsigned long long>(start_sector),
                        static_cast<unsigned long long>(sectors),
                        static_cast<unsigned int>(partition_base[11]),
                        static_cast<unsigned int>(partition_base[12]),
                        static_cast<unsigned int>(partition_base[510]),
                        static_cast<unsigned int>(partition_base[511]));

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
