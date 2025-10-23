#include "fat32.hpp"

#include <stddef.h>
#include <stdint.h>

#include "drivers/log/logging.hpp"
#include "lib/mem.hpp"

namespace {

struct __attribute__((packed)) BpbFat32 {
    uint8_t jump[3];
    char oem_name[8];
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sector_count;
    uint8_t num_fats;
    uint16_t root_entry_count;
    uint16_t total_sectors_16;
    uint8_t media;
    uint16_t fat_size_16;
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    uint32_t fat_size_32;
    uint16_t ext_flags;
    uint16_t fs_version;
    uint32_t root_cluster;
    uint16_t fs_info;
    uint16_t bk_boot_sector;
    uint8_t reserved[12];
    uint8_t drive_number;
    uint8_t reserved1;
    uint8_t boot_signature;
    uint32_t volume_id;
    char volume_label[11];
    char fs_type[8];
};

constexpr uint8_t ATTR_LONG_NAME = 0x0F;
constexpr uint8_t ATTR_DIRECTORY = 0x10;
constexpr uint8_t ATTR_VOLUME_ID = 0x08;

alignas(512) uint8_t sector_buffer[512];
alignas(512) uint8_t cluster_buffer[4096];
alignas(512) uint8_t fat_cache[512];
uint32_t fat_cache_sector = 0xFFFFFFFF;
const fs::BlockDevice* fat_cache_device = nullptr;

inline uint32_t cluster_to_lba(const Fat32Volume& vol, uint32_t cluster) {
    return vol.cluster_begin_lba +
           ((cluster - 2) * vol.sectors_per_cluster);
}

bool read_sector(const fs::BlockDevice& device, uint32_t lba, void* buffer) {
    fs::BlockIoStatus status = fs::block_read(device, lba, 1, buffer);
    if (status != fs::BlockIoStatus::Ok) {
        log_message(LogLevel::Error,
                    "FAT32: failed to read sector %u (status %d)",
                    lba, static_cast<int>(status));
        return false;
    }
    return true;
}

bool read_sectors(const fs::BlockDevice& device, uint32_t lba, uint8_t count,
                  void* buffer) {
    fs::BlockIoStatus status = fs::block_read(device, lba, count, buffer);
    if (status != fs::BlockIoStatus::Ok) {
        log_message(LogLevel::Error,
                    "FAT32: failed to read sectors %u (+%u) (status %d)",
                    lba, count, static_cast<int>(status));
        return false;
    }
    return true;
}

uint32_t read_fat_entry(Fat32Volume& volume, uint32_t cluster) {
    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sector = volume.fat_begin_lba + (fat_offset / 512);
    uint32_t within_sector = fat_offset % 512;

    if (fat_cache_sector != fat_sector || fat_cache_device != &volume.device) {
        if (!read_sector(volume.device, fat_sector, fat_cache)) {
            return 0x0FFFFFFF;
        }
        fat_cache_sector = fat_sector;
        fat_cache_device = &volume.device;
    }

    uint32_t value = *reinterpret_cast<uint32_t*>(fat_cache + within_sector);
    return value & 0x0FFFFFFF;
}

void format_83_name(const uint8_t* raw, char* out) {
    int pos = 0;
    for (int i = 0; i < 8; ++i) {
        if (raw[i] == ' ') continue;
        out[pos++] = static_cast<char>(raw[i]);
    }
    int ext_len = 0;
    for (int i = 8; i < 11; ++i) {
        if (raw[i] != ' ') ext_len++;
    }
    if (ext_len > 0) {
        out[pos++] = '.';
        for (int i = 8; i < 11; ++i) {
            if (raw[i] == ' ') continue;
            out[pos++] = static_cast<char>(raw[i]);
        }
    }
    out[pos] = '\0';
}

bool names_equal(const char* a, const char* b) {
    while (*a && *b) {
        char ca = (*a >= 'a' && *a <= 'z') ? *a - ('a' - 'A') : *a;
        char cb = (*b >= 'a' && *b <= 'z') ? *b - ('a' - 'A') : *b;
        if (ca != cb) {
            return false;
        }
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

}  // namespace

bool fat32_mount(Fat32Volume& volume, const fs::BlockDevice& device) {
    volume.device = device;
    volume.mounted = false;
    const char* device_name =
        volume.device.name != nullptr ? volume.device.name : "(unnamed)";

    if (volume.device.sector_size != 512) {
        log_message(LogLevel::Warn,
                    "FAT32: unsupported sector size %zu on device %s",
                    volume.device.sector_size, device_name);
        return false;
    }

    if (!read_sector(volume.device, 0, sector_buffer)) {
        return false;
    }

    auto* bpb = reinterpret_cast<BpbFat32*>(sector_buffer);
    if (bpb->bytes_per_sector != 512) {
        log_message(LogLevel::Warn,
                    "FAT32: unsupported bytes per sector: %u",
                    bpb->bytes_per_sector);
        return false;
    }
    if (bpb->sectors_per_cluster == 0) {
        log_message(LogLevel::Warn, "FAT32: invalid sectors per cluster");
        return false;
    }
    if (bpb->fat_size_32 == 0) {
        log_message(LogLevel::Warn, "FAT32: invalid fat size");
        return false;
    }
    if (bpb->fat_size_16 != 0 || bpb->root_entry_count != 0) {
        log_message(LogLevel::Warn,
                    "FAT32: volume reports FAT16 parameters (root entries=%u fat16=%u)",
                    bpb->root_entry_count, bpb->fat_size_16);
        return false;
    }
    if (bpb->total_sectors_32 == 0) {
        log_message(LogLevel::Warn, "FAT32: invalid total sectors");
        return false;
    }
    if (bpb->root_cluster < 2) {
        log_message(LogLevel::Warn, "FAT32: invalid root cluster: %u",
                    bpb->root_cluster);
        return false;
    }

    volume.sectors_per_cluster = bpb->sectors_per_cluster;
    volume.reserved_sectors = bpb->reserved_sector_count;
    volume.fat_size_sectors = bpb->fat_size_32;
    volume.fat_begin_lba = volume.reserved_sectors;
    volume.cluster_begin_lba =
        volume.fat_begin_lba + (bpb->num_fats * volume.fat_size_sectors);
    volume.root_dir_first_cluster = bpb->root_cluster;

    fat_cache_sector = 0xFFFFFFFF;
    fat_cache_device = &volume.device;

    volume.mounted = true;

    log_message(LogLevel::Info,
                "FAT32: mounted %s root cluster=%u spc=%u reserved=%u fat=%u",
                device_name, volume.root_dir_first_cluster,
                volume.sectors_per_cluster, volume.reserved_sectors,
                volume.fat_size_sectors);

    return true;
}

size_t fat32_list_root(Fat32Volume& volume, Fat32DirEntry* out_entries,
                       size_t max_entries) {
    if (!volume.mounted || out_entries == nullptr || max_entries == 0) {
        return 0;
    }

    uint32_t current_cluster = volume.root_dir_first_cluster;
    size_t count = 0;
    bool done = false;

    while (!done) {
        uint32_t lba = cluster_to_lba(volume, current_cluster);
        if (!read_sectors(volume.device, lba, volume.sectors_per_cluster,
                          cluster_buffer)) {
            break;
        }

        size_t entries_in_cluster =
            (volume.sectors_per_cluster * 512) / 32;
        for (size_t i = 0; i < entries_in_cluster; ++i) {
            auto* entry =
                reinterpret_cast<const uint8_t*>(cluster_buffer + i * 32);

            if (entry[0] == 0x00) {
                done = true;
                break;
            }
            if (entry[0] == 0xE5 || entry[11] == ATTR_LONG_NAME ||
                (entry[11] & ATTR_VOLUME_ID)) {
                continue;
            }

            if (count < max_entries) {
                Fat32DirEntry& out = out_entries[count++];
                memset(&out, 0, sizeof(Fat32DirEntry));
                format_83_name(entry, out.name);
                out.attributes = entry[11];
                out.first_cluster =
                    (static_cast<uint32_t>(entry[20]) << 16) |
                    (static_cast<uint32_t>(entry[21]) << 24) |
                    (static_cast<uint32_t>(entry[26])) |
                    (static_cast<uint32_t>(entry[27]) << 8);
                out.size = *reinterpret_cast<const uint32_t*>(entry + 28);
            }
        }

        uint32_t next = read_fat_entry(volume, current_cluster);
        if (next >= 0x0FFFFFF8) {
            break;
        }
        if (next == 0x0FFFFFF7) {
            log_message(LogLevel::Warn, "FAT32: bad cluster %u", next);
            break;
        }
        current_cluster = next;
    }

    return count;
}

bool fat32_read_file(Fat32Volume& volume, const char* name, void* buffer,
                     size_t buffer_size, size_t& out_size) {
    out_size = 0;
    if (!volume.mounted || name == nullptr || buffer == nullptr) {
        return false;
    }

    Fat32DirEntry entries[64];
    size_t count = fat32_list_root(volume, entries, 64);
    for (size_t i = 0; i < count; ++i) {
        if (names_equal(entries[i].name, name)) {
            uint32_t cluster = entries[i].first_cluster;
            uint32_t remaining = entries[i].size;
            uint8_t* out_ptr = static_cast<uint8_t*>(buffer);

            while (remaining > 0) {
                uint32_t lba = cluster_to_lba(volume, cluster);
                uint32_t to_read =
                    volume.sectors_per_cluster * 512;
                if (to_read > remaining) {
                    to_read = remaining;
                }

                if (!read_sectors(volume.device, lba,
                                  volume.sectors_per_cluster,
                                  cluster_buffer)) {
                    return false;
                }

                if (out_size + to_read > buffer_size) {
                    log_message(LogLevel::Warn,
                                "FAT32: buffer too small for file");
                    return false;
                }

                memcpy(out_ptr, cluster_buffer, to_read);
                out_ptr += to_read;
                out_size += to_read;
                remaining -= to_read;

                if (remaining == 0) {
                    break;
                }

                uint32_t next = read_fat_entry(volume, cluster);
                if (next >= 0x0FFFFFF8) {
                    break;
                }
                if (next == 0x0FFFFFF7) {
                    log_message(LogLevel::Warn, "FAT32: bad cluster in chain");
                    return false;
                }
                cluster = next;
            }

            return true;
        }
    }

    log_message(LogLevel::Warn, "FAT32: file '%s' not found", name);
    return false;
}
