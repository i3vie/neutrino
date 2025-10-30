#include "fat32.hpp"

#include <stddef.h>
#include <stdint.h>

#include "drivers/log/logging.hpp"
#include "fs/vfs.hpp"
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
constexpr size_t kMaxSegmentLength = 32;

constexpr uint32_t kFatEntryMask = 0x0FFFFFFF;
constexpr uint32_t kFatEoc = 0x0FFFFFF8;
constexpr uint32_t kFatBadCluster = 0x0FFFFFF7;
constexpr uint32_t kFatFreeCluster = 0;

constexpr uint32_t kFsInfoLeadSignature = 0x41615252;
constexpr uint32_t kFsInfoStructSignature = 0x61417272;
constexpr uint32_t kFsInfoTrailSignature = 0xAA550000;

alignas(512) uint8_t sector_buffer[512];
alignas(512) uint8_t cluster_buffer[32768];
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

bool write_sector(const fs::BlockDevice& device, uint32_t lba,
                  const void* buffer) {
    fs::BlockIoStatus status = fs::block_write(device, lba, 1, buffer);
    if (status != fs::BlockIoStatus::Ok) {
        log_message(LogLevel::Error,
                    "FAT32: failed to write sector %u (status %d)",
                    lba, static_cast<int>(status));
        return false;
    }
    return true;
}

bool write_sectors(const fs::BlockDevice& device, uint32_t lba, uint8_t count,
                   const void* buffer) {
    fs::BlockIoStatus status = fs::block_write(device, lba, count, buffer);
    if (status != fs::BlockIoStatus::Ok) {
        log_message(LogLevel::Error,
                    "FAT32: failed to write sectors %u (+%u) (status %d)",
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
    return value & kFatEntryMask;
}

bool write_fat_entry(Fat32Volume& volume, uint32_t cluster,
                     uint32_t value) {
    uint32_t fat_offset = cluster * 4;
    uint32_t sector_index = fat_offset / 512;
    uint32_t within_sector = fat_offset % 512;
    uint32_t fat_sector = volume.fat_begin_lba + sector_index;

    if (!read_sector(volume.device, fat_sector, fat_cache)) {
        return false;
    }

    uint32_t original =
        *reinterpret_cast<uint32_t*>(fat_cache + within_sector);
    uint32_t new_value =
        (original & 0xF0000000u) | (value & kFatEntryMask);
    *reinterpret_cast<uint32_t*>(fat_cache + within_sector) = new_value;

    if (!write_sector(volume.device, fat_sector, fat_cache)) {
        return false;
    }

    // Update cache to reflect new value.
    fat_cache_sector = fat_sector;
    fat_cache_device = &volume.device;

    // Mirror to additional FATs if present.
    for (uint32_t fat = 1; fat < volume.num_fats; ++fat) {
        uint32_t mirror_sector =
            volume.fat_begin_lba + (fat * volume.fat_size_sectors) +
            sector_index;
        if (!write_sector(volume.device, mirror_sector, fat_cache)) {
            return false;
        }
    }

    return true;
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

size_t string_length(const char* str) {
    size_t len = 0;
    if (str == nullptr) {
        return 0;
    }
    while (str[len] != '\0') {
        ++len;
    }
    return len;
}

bool build_short_name(const char* name, uint8_t (&out)[11]) {
    if (name == nullptr || name[0] == '\0') {
        return false;
    }

    for (size_t i = 0; i < 11; ++i) {
        out[i] = ' ';
    }

    size_t base_len = 0;
    size_t ext_len = 0;
    bool seen_dot = false;

    for (size_t i = 0; name[i] != '\0'; ++i) {
        char ch = name[i];
        if (ch == '.') {
            if (seen_dot) {
                return false;
            }
            seen_dot = true;
            if (i == 0) {
                return false;
            }
            continue;
        }
        if (ch == ' ' || ch == '\t') {
            return false;
        }
        if (ch >= 'a' && ch <= 'z') {
            ch = static_cast<char>(ch - ('a' - 'A'));
        }
        bool valid = (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
                     ch == '_' || ch == '-';
        if (!valid) {
            return false;
        }

        if (!seen_dot) {
            if (base_len >= 8) {
                return false;
            }
            out[base_len++] = static_cast<uint8_t>(ch);
        } else {
            if (ext_len >= 3) {
                return false;
            }
            out[8 + ext_len++] = static_cast<uint8_t>(ch);
        }
    }

    if (base_len == 0) {
        return false;
    }
    if (seen_dot && ext_len == 0) {
        return false;
    }
    return true;
}

size_t entries_per_cluster(const Fat32Volume& volume) {
    return (volume.sectors_per_cluster * 512u) / 32u;
}

enum class IterationResult {
    Continue,
    StopSuccess,
    StopFailure,
};

template <typename Fn>
bool iterate_directory(Fat32Volume& volume, uint32_t start_cluster, Fn&& fn) {
    uint32_t current_cluster = start_cluster;
    bool done = false;
    size_t raw_index = 0;
    size_t visible_index = 0;

    while (!done) {
        uint32_t lba = cluster_to_lba(volume, current_cluster);
        if (!read_sectors(volume.device, lba, volume.sectors_per_cluster,
                          cluster_buffer)) {
            return false;
        }

        size_t count = entries_per_cluster(volume);
        for (size_t i = 0; i < count; ++i) {
            const uint8_t* entry =
                reinterpret_cast<const uint8_t*>(cluster_buffer + i * 32);

            if (entry[0] == 0x00) {
                done = true;
                break;
            }

            bool skip = (entry[0] == 0xE5) ||
                        (entry[11] == ATTR_LONG_NAME) ||
                        ((entry[11] & ATTR_VOLUME_ID) != 0);

            if (!skip) {
                IterationResult res = fn(entry,
                                         current_cluster,
                                         static_cast<uint32_t>(raw_index),
                                         static_cast<uint32_t>(visible_index));
                if (res == IterationResult::StopSuccess) {
                    return true;
                }
                if (res == IterationResult::StopFailure) {
                    return false;
                }
                ++visible_index;
            }

            ++raw_index;
        }

        if (done) {
            break;
        }

        uint32_t next = read_fat_entry(volume, current_cluster);
        if (next >= kFatEoc) {
            break;
        }
        if (next == kFatBadCluster) {
            log_message(LogLevel::Warn, "FAT32: bad cluster %u", next);
            return false;
        }
        current_cluster = next;
    }

    return true;
}

void copy_entry(const uint8_t* raw,
                uint32_t directory_cluster,
                uint32_t raw_index,
                Fat32DirEntry& out) {
    memset(&out, 0, sizeof(Fat32DirEntry));
    format_83_name(raw, out.name);
    out.attributes = raw[11];
    out.first_cluster =
        (static_cast<uint32_t>(raw[20]) << 16) |
        (static_cast<uint32_t>(raw[21]) << 24) |
        (static_cast<uint32_t>(raw[26])) |
        (static_cast<uint32_t>(raw[27]) << 8);
    out.size = *reinterpret_cast<const uint32_t*>(raw + 28);
    out.directory_cluster = directory_cluster;
    out.raw_entry_index = raw_index;
}

void initialize_next_free(Fat32Volume& volume) {
    volume.next_free_cluster = 2;

    if (volume.fs_info_sector == 0) {
        return;
    }
    if (volume.fs_info_sector >= volume.total_sectors) {
        return;
    }

    if (!read_sector(volume.device, volume.fs_info_sector, sector_buffer)) {
        log_message(LogLevel::Warn,
                    "FAT32: failed to read FSINFO, using default allocator");
        return;
    }

    uint32_t lead =
        *reinterpret_cast<const uint32_t*>(sector_buffer + 0);
    uint32_t struct_sig =
        *reinterpret_cast<const uint32_t*>(sector_buffer + 484);
    uint32_t trail =
        *reinterpret_cast<const uint32_t*>(sector_buffer + 508);

    if (lead != kFsInfoLeadSignature ||
        struct_sig != kFsInfoStructSignature ||
        trail != kFsInfoTrailSignature) {
        return;
    }

    uint32_t next_free =
        *reinterpret_cast<const uint32_t*>(sector_buffer + 492);
    uint32_t max_cluster = volume.total_clusters + 1;
    if (next_free >= 2 && next_free <= max_cluster) {
        volume.next_free_cluster = next_free;
    }
}

bool clear_cluster(Fat32Volume& volume, uint32_t cluster) {
    size_t bytes =
        static_cast<size_t>(volume.sectors_per_cluster) * 512u;
    if (bytes > sizeof(cluster_buffer)) {
        log_message(LogLevel::Warn,
                    "FAT32: cluster buffer too small for cluster %u",
                    cluster);
        return false;
    }
    memset(cluster_buffer, 0, bytes);
    uint32_t lba = cluster_to_lba(volume, cluster);
    return write_sectors(volume.device,
                         lba,
                         volume.sectors_per_cluster,
                         cluster_buffer);
}

bool allocate_cluster(Fat32Volume& volume, uint32_t& out_cluster) {
    if (volume.total_clusters == 0) {
        log_message(LogLevel::Warn, "FAT32: no clusters available");
        return false;
    }

    uint32_t max_cluster = volume.total_clusters + 1;
    uint32_t start = volume.next_free_cluster;
    if (start < 2 || start > max_cluster) {
        start = 2;
    }

    uint32_t cluster = start;
    do {
        uint32_t status = read_fat_entry(volume, cluster);
        if (status == kFatFreeCluster) {
            if (!write_fat_entry(volume, cluster, kFatEoc)) {
                return false;
            }
            if (!clear_cluster(volume, cluster)) {
                return false;
            }
            out_cluster = cluster;

            uint32_t next_candidate = cluster + 1;
            if (next_candidate > max_cluster) {
                next_candidate = 2;
            }
            volume.next_free_cluster = next_candidate;
            return true;
        }

        ++cluster;
        if (cluster > max_cluster) {
            cluster = 2;
        }
    } while (cluster != start);

    log_message(LogLevel::Warn, "FAT32: out of free clusters");
    return false;
}

bool get_chain_info(Fat32Volume& volume,
                    uint32_t first_cluster,
                    uint32_t& out_length,
                    uint32_t& out_tail) {
    out_length = 0;
    out_tail = 0;

    if (first_cluster < 2) {
        return true;
    }

    uint32_t max_cluster = volume.total_clusters + 1;
    if (first_cluster > max_cluster) {
        log_message(LogLevel::Warn,
                    "FAT32: invalid first cluster %u", first_cluster);
        return false;
    }

    uint32_t cluster = first_cluster;
    uint32_t traversed = 0;
    while (cluster >= 2 && cluster <= max_cluster) {
        ++traversed;
        out_tail = cluster;

        if (traversed > volume.total_clusters + 1) {
            log_message(LogLevel::Warn, "FAT32: cluster chain loop detected");
            return false;
        }

        uint32_t next = read_fat_entry(volume, cluster);
        if (next == kFatBadCluster) {
            log_message(LogLevel::Warn, "FAT32: bad cluster in chain");
            return false;
        }
        if (next >= kFatEoc) {
            break;
        }
        if (next < 2 || next > max_cluster) {
            log_message(LogLevel::Warn,
                        "FAT32: cluster %u points to invalid next %u",
                        cluster, next);
            return false;
        }

        cluster = next;
    }

    out_length = traversed;
    return true;
}

bool ensure_cluster_count(Fat32Volume& volume,
                          Fat32DirEntry& entry,
                          uint32_t required_clusters) {
    if (required_clusters == 0) {
        return true;
    }

    uint32_t length = 0;
    uint32_t tail = 0;
    if (!get_chain_info(volume, entry.first_cluster, length, tail)) {
        return false;
    }

    if (length == 0) {
        uint32_t new_cluster = 0;
        if (!allocate_cluster(volume, new_cluster)) {
            return false;
        }
        entry.first_cluster = new_cluster;
        tail = new_cluster;
        length = 1;
    }

    while (length < required_clusters) {
        uint32_t new_cluster = 0;
        if (!allocate_cluster(volume, new_cluster)) {
            return false;
        }
        if (!write_fat_entry(volume, tail, new_cluster)) {
            return false;
        }
        tail = new_cluster;
        ++length;
    }

    return true;
}

bool get_cluster_at_index(Fat32Volume& volume,
                          uint32_t first_cluster,
                          uint32_t index,
                          uint32_t& out_cluster) {
    if (first_cluster < 2) {
        return false;
    }

    uint32_t cluster = first_cluster;
    for (uint32_t i = 0; i < index; ++i) {
        uint32_t next = read_fat_entry(volume, cluster);
        if (next == kFatBadCluster) {
            log_message(LogLevel::Warn, "FAT32: bad cluster in chain");
            return false;
        }
        if (next >= kFatEoc) {
            return false;
        }
        cluster = next;
        if (i > volume.total_clusters + 1) {
            log_message(LogLevel::Warn, "FAT32: cluster traversal overflow");
            return false;
        }
    }

    out_cluster = cluster;
    return true;
}

bool zero_range(Fat32Volume& volume,
                Fat32DirEntry& entry,
                uint32_t start,
                uint32_t length) {
    if (length == 0) {
        return true;
    }

    size_t cluster_size =
        static_cast<size_t>(volume.sectors_per_cluster) * 512u;
    if (cluster_size == 0) {
        return false;
    }
    if (cluster_size > sizeof(cluster_buffer)) {
        log_message(LogLevel::Warn,
                    "FAT32: cluster size %zu exceeds buffer capacity",
                    cluster_size);
        return false;
    }
    if (cluster_size > sizeof(cluster_buffer)) {
        log_message(LogLevel::Warn,
                    "FAT32: cluster size %zu exceeds buffer capacity",
                    cluster_size);
        return false;
    }

    uint32_t end = start + length;
    uint32_t offset = start;

    while (offset < end) {
        uint32_t cluster_index =
            offset / static_cast<uint32_t>(cluster_size);
        uint32_t cluster_offset =
            offset % static_cast<uint32_t>(cluster_size);
        uint32_t cluster_number = 0;
        if (!get_cluster_at_index(volume,
                                  entry.first_cluster,
                                  cluster_index,
                                  cluster_number)) {
            return false;
        }

        size_t chunk = cluster_size - cluster_offset;
        uint32_t remaining = end - offset;
        if (chunk > remaining) {
            chunk = remaining;
        }

        uint32_t lba = cluster_to_lba(volume, cluster_number);
        if (chunk == cluster_size && cluster_offset == 0) {
            memset(cluster_buffer, 0, cluster_size);
            if (!write_sectors(volume.device,
                               lba,
                               volume.sectors_per_cluster,
                               cluster_buffer)) {
                return false;
            }
        } else {
            if (!read_sectors(volume.device,
                              lba,
                              volume.sectors_per_cluster,
                              cluster_buffer)) {
                return false;
            }
            memset(cluster_buffer + cluster_offset, 0, chunk);
            if (!write_sectors(volume.device,
                               lba,
                               volume.sectors_per_cluster,
                               cluster_buffer)) {
                return false;
            }
        }

        offset += static_cast<uint32_t>(chunk);
    }

    return true;
}

bool update_directory_entry(Fat32Volume& volume,
                            const Fat32DirEntry& entry) {
    uint32_t entries_per_cluster32 =
        static_cast<uint32_t>(entries_per_cluster(volume));
    if (entries_per_cluster32 == 0) {
        return false;
    }

    uint32_t raw_index = entry.raw_entry_index;
    uint32_t cluster_offset = raw_index / entries_per_cluster32;
    uint32_t index_in_cluster = raw_index % entries_per_cluster32;

    uint32_t cluster = entry.directory_cluster;
    if (cluster < 2) {
        return false;
    }

    for (uint32_t i = 0; i < cluster_offset; ++i) {
        uint32_t next = read_fat_entry(volume, cluster);
        if (next == kFatBadCluster) {
            log_message(LogLevel::Warn, "FAT32: bad cluster in directory chain");
            return false;
        }
        if (next >= kFatEoc) {
            log_message(LogLevel::Warn,
                        "FAT32: directory chain too short for entry update");
            return false;
        }
        cluster = next;
    }

    uint32_t lba = cluster_to_lba(volume, cluster);
    uint32_t entry_byte = index_in_cluster * 32;
    uint32_t sector_offset = entry_byte / 512;
    uint32_t byte_offset = entry_byte % 512;

    if (!read_sector(volume.device, lba + sector_offset, sector_buffer)) {
        return false;
    }

    uint8_t* raw = sector_buffer + byte_offset;
    uint32_t first_cluster = entry.first_cluster;
    uint16_t high = static_cast<uint16_t>(first_cluster >> 16);
    uint16_t low = static_cast<uint16_t>(first_cluster & 0xFFFF);

    raw[20] = static_cast<uint8_t>(high & 0xFF);
    raw[21] = static_cast<uint8_t>((high >> 8) & 0xFF);
    raw[26] = static_cast<uint8_t>(low & 0xFF);
    raw[27] = static_cast<uint8_t>((low >> 8) & 0xFF);

    *reinterpret_cast<uint32_t*>(raw + 28) = entry.size;

    return write_sector(volume.device, lba + sector_offset, sector_buffer);
}

const char* trim_leading_slashes(const char* path) {
    if (path == nullptr) {
        return nullptr;
    }
    while (*path == '/') {
        ++path;
    }
    return path;
}

bool next_segment(const char*& cursor, char* segment, bool& has_more) {
    cursor = trim_leading_slashes(cursor);
    if (cursor == nullptr || *cursor == '\0') {
        return false;
    }

    size_t len = 0;
    while (cursor[len] != '\0' && cursor[len] != '/') {
        if (len + 1 >= kMaxSegmentLength) {
            return false;
        }
        segment[len] = cursor[len];
        ++len;
    }
    segment[len] = '\0';

    const char* next = cursor + len;
    while (*next == '/') {
        ++next;
    }
    has_more = (*next != '\0');
    cursor = next;
    return true;
}

bool resolve_directory_cluster(Fat32Volume& volume,
                               const char* path,
                               uint32_t& out_cluster) {
    uint32_t current = volume.root_dir_first_cluster;
    if (path == nullptr) {
        out_cluster = current;
        return true;
    }

    const char* cursor = path;
    char segment[kMaxSegmentLength]{};
    bool has_more = false;

    while (next_segment(cursor, segment, has_more)) {
        Fat32DirEntry entry{};
        if (!fat32_find_entry(volume, current, segment, entry)) {
            return false;
        }
        if ((entry.attributes & ATTR_DIRECTORY) == 0) {
            return false;
        }
        current = entry.first_cluster;
        if (!has_more) {
            break;
        }
    }

    const char* remaining = trim_leading_slashes(cursor);
    if (remaining != nullptr && *remaining != '\0') {
        return false;
    }

    out_cluster = current;
    return true;
}

bool resolve_entry(Fat32Volume& volume,
                   const char* path,
                   Fat32DirEntry& out_entry) {
    if (path == nullptr) {
        return false;
    }

    const char* cursor = path;
    char segment[kMaxSegmentLength]{};
    bool has_more = false;
    uint32_t current_cluster = volume.root_dir_first_cluster;

    while (next_segment(cursor, segment, has_more)) {
        Fat32DirEntry entry{};
        if (!fat32_find_entry(volume, current_cluster, segment, entry)) {
            return false;
        }
        if (!has_more) {
            out_entry = entry;
            return true;
        }
        if ((entry.attributes & ATTR_DIRECTORY) == 0) {
            return false;
        }
        current_cluster = entry.first_cluster;
    }
    return false;
}

struct DirectorySlot {
    uint32_t cluster;
    uint32_t entry_index;
    uint32_t raw_index;
    bool was_end_marker;
};

bool split_parent_and_name(Fat32Volume& volume,
                           const char* path,
                           uint32_t& out_parent_cluster,
                           char (&out_name)[kMaxSegmentLength]) {
    if (path == nullptr) {
        return false;
    }

    const char* cursor = path;
    char segment[kMaxSegmentLength]{};
    bool has_more = false;
    uint32_t current_cluster = volume.root_dir_first_cluster;

    while (next_segment(cursor, segment, has_more)) {
        if (!has_more) {
            if (segment[0] == '\0') {
                return false;
            }
            size_t len = string_length(segment);
            if (len >= kMaxSegmentLength) {
                return false;
            }
            for (size_t i = 0; i < len; ++i) {
                out_name[i] = segment[i];
            }
            out_name[len] = '\0';
            out_parent_cluster = current_cluster;
            return true;
        }

        Fat32DirEntry entry{};
        if (!fat32_find_entry(volume, current_cluster, segment, entry)) {
            return false;
        }
        if ((entry.attributes & ATTR_DIRECTORY) == 0) {
            return false;
        }
        current_cluster = entry.first_cluster;
    }
    return false;
}

bool find_directory_slot(Fat32Volume& volume,
                         uint32_t directory_cluster,
                         DirectorySlot& out_slot) {
    const size_t cluster_entries = entries_per_cluster(volume);
    if (cluster_entries == 0) {
        return false;
    }

    size_t cluster_size =
        static_cast<size_t>(volume.sectors_per_cluster) * 512u;
    if (cluster_size > sizeof(cluster_buffer)) {
        log_message(LogLevel::Warn,
                    "FAT32: cluster size %zu exceeds buffer capacity",
                    cluster_size);
        return false;
    }

    uint32_t current_cluster = directory_cluster;
    uint32_t raw_index = 0;

    while (true) {
        if (!read_sectors(volume.device,
                          cluster_to_lba(volume, current_cluster),
                          volume.sectors_per_cluster,
                          cluster_buffer)) {
            return false;
        }

        for (size_t i = 0; i < cluster_entries; ++i, ++raw_index) {
            uint8_t* entry =
                reinterpret_cast<uint8_t*>(cluster_buffer + i * 32);
            uint8_t first = entry[0];
            if (first == 0x00 || first == 0xE5) {
                out_slot.cluster = current_cluster;
                out_slot.entry_index = static_cast<uint32_t>(i);
                out_slot.raw_index = raw_index;
                out_slot.was_end_marker = (first == 0x00);
                return true;
            }
        }

        uint32_t next = read_fat_entry(volume, current_cluster);
        if (next == kFatBadCluster) {
            log_message(LogLevel::Warn,
                        "FAT32: directory cluster chain corrupt");
            return false;
        }
        if (next >= kFatEoc) {
            uint32_t new_cluster = 0;
            if (!allocate_cluster(volume, new_cluster)) {
                return false;
            }
            if (!write_fat_entry(volume, current_cluster, new_cluster)) {
                return false;
            }
            current_cluster = new_cluster;
            memset(cluster_buffer, 0, cluster_size);
            out_slot.cluster = current_cluster;
            out_slot.entry_index = 0;
            out_slot.raw_index = raw_index;
            out_slot.was_end_marker = true;
            return true;
        }
        current_cluster = next;
    }
}

bool fat32_create_file(Fat32Volume& volume,
                       const char* path,
                       Fat32DirEntry& out_entry) {
    if (!volume.mounted || path == nullptr) {
        return false;
    }

    char name[kMaxSegmentLength]{};
    uint32_t parent_cluster = 0;
    if (!split_parent_and_name(volume, path, parent_cluster, name)) {
        return false;
    }

    Fat32DirEntry existing{};
    if (fat32_find_entry(volume, parent_cluster, name, existing)) {
        return false;
    }

    uint8_t short_name[11];
    if (!build_short_name(name, short_name)) {
        return false;
    }

    DirectorySlot slot{};
    if (!find_directory_slot(volume, parent_cluster, slot)) {
        return false;
    }

    const size_t cluster_entries = entries_per_cluster(volume);
    size_t cluster_size =
        static_cast<size_t>(volume.sectors_per_cluster) * 512u;
    if (cluster_size > sizeof(cluster_buffer)) {
        log_message(LogLevel::Warn,
                    "FAT32: cluster size %zu exceeds buffer capacity",
                    cluster_size);
        return false;
    }

    uint8_t* raw =
        reinterpret_cast<uint8_t*>(cluster_buffer + slot.entry_index * 32);
    memset(raw, 0, 32);
    for (size_t i = 0; i < 11; ++i) {
        raw[i] = short_name[i];
    }
    raw[11] = 0x00;  // regular file attributes
    *reinterpret_cast<uint32_t*>(raw + 28) = 0;

    if (slot.was_end_marker) {
        size_t next_index = slot.entry_index + 1;
        if (next_index < cluster_entries) {
            uint8_t* next_entry =
                reinterpret_cast<uint8_t*>(cluster_buffer + next_index * 32);
            next_entry[0] = 0x00;
        }
    }

    if (!write_sectors(volume.device,
                       cluster_to_lba(volume, slot.cluster),
                       volume.sectors_per_cluster,
                       cluster_buffer)) {
        return false;
    }

    copy_entry(raw, slot.cluster, slot.raw_index, out_entry);
    return true;
}

void to_vfs_entry(const Fat32DirEntry& source, vfs::DirEntry& dest) {
    memset(&dest, 0, sizeof(dest));
    size_t i = 0;
    while (source.name[i] != '\0' && i + 1 < sizeof(dest.name)) {
        dest.name[i] = source.name[i];
        ++i;
    }
    dest.name[i] = '\0';
    dest.flags = ((source.attributes & ATTR_DIRECTORY) != 0)
                     ? vfs::kDirEntryFlagDirectory
                     : 0u;
    dest.reserved = 0;
    dest.size = source.size;
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
    if (bpb->num_fats == 0) {
        log_message(LogLevel::Warn, "FAT32: volume reports zero FATs");
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
    volume.num_fats = bpb->num_fats;
    volume.fs_info_sector = bpb->fs_info;

    uint32_t total_sectors =
        (bpb->total_sectors_32 != 0) ? bpb->total_sectors_32
                                     : static_cast<uint32_t>(bpb->total_sectors_16);
    volume.total_sectors = total_sectors;

    uint32_t fats_total =
        static_cast<uint32_t>(volume.num_fats) * volume.fat_size_sectors;
    uint32_t overhead = volume.reserved_sectors + fats_total;
    if (overhead >= total_sectors) {
        log_message(LogLevel::Warn, "FAT32: invalid volume geometry");
        return false;
    }
    uint32_t data_sectors = total_sectors - overhead;
    uint32_t total_clusters =
        data_sectors / static_cast<uint32_t>(volume.sectors_per_cluster);
    if (total_clusters == 0) {
        log_message(LogLevel::Warn, "FAT32: no data clusters available");
        return false;
    }
    volume.total_clusters = total_clusters;
    initialize_next_free(volume);

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

bool fat32_list_directory(Fat32Volume& volume, uint32_t start_cluster,
                          Fat32DirEntry* out_entries, size_t max_entries,
                          size_t& out_count) {
    out_count = 0;
    if (!volume.mounted || out_entries == nullptr || max_entries == 0) {
        return false;
    }

    size_t collected = 0;
    bool ok = iterate_directory(volume, start_cluster,
                                [&](const uint8_t* entry,
                                    uint32_t dir_cluster,
                                    uint32_t raw_index,
                                    uint32_t) -> IterationResult {
        if (collected < max_entries) {
            copy_entry(entry, dir_cluster, raw_index, out_entries[collected]);
        }
        ++collected;
        return IterationResult::Continue;
    });

    if (!ok) {
        return false;
    }

    out_count = (collected > max_entries) ? max_entries : collected;
    return true;
}

bool fat32_find_entry(Fat32Volume& volume, uint32_t directory_cluster,
                      const char* name, Fat32DirEntry& out_entry) {
    if (!volume.mounted || name == nullptr) {
        return false;
    }

    bool found = false;
    bool ok = iterate_directory(volume, directory_cluster,
                                [&](const uint8_t* raw,
                                    uint32_t dir_cluster,
                                    uint32_t raw_index,
                                    uint32_t) -> IterationResult {
        char entry_name[12] = {0};
        format_83_name(raw, entry_name);
        if (names_equal(entry_name, name)) {
            copy_entry(raw, dir_cluster, raw_index, out_entry);
            found = true;
            return IterationResult::StopSuccess;
        }
        return IterationResult::Continue;
    });

    return ok && found;
}

bool fat32_read_file(Fat32Volume& volume, const Fat32DirEntry& entry,
                     void* buffer, size_t buffer_size, size_t& out_size) {
    return fat32_read_file_range(volume, entry, 0, buffer, buffer_size,
                                 out_size);
}

bool fat32_read_file_range(Fat32Volume& volume, const Fat32DirEntry& entry,
                           uint32_t offset, void* buffer, size_t buffer_size,
                           size_t& out_size) {
    out_size = 0;
    if (!volume.mounted || buffer == nullptr) {
        return false;
    }
    if ((entry.attributes & ATTR_DIRECTORY) != 0) {
        log_message(LogLevel::Warn, "FAT32: attempt to read directory");
        return false;
    }

    if (offset >= entry.size) {
        return true;
    }

    uint32_t cluster = entry.first_cluster;
    uint32_t consumed = 0;
    const size_t cluster_size = static_cast<size_t>(volume.sectors_per_cluster) *
                                512u;
    if (cluster_size > sizeof(cluster_buffer)) {
        log_message(LogLevel::Warn,
                    "FAT32: cluster size %zu exceeds buffer capacity",
                    cluster_size);
        return false;
    }
    uint8_t* out_ptr = static_cast<uint8_t*>(buffer);

    while (consumed < entry.size) {
        uint32_t lba = cluster_to_lba(volume, cluster);
        if (!read_sectors(volume.device, lba, volume.sectors_per_cluster,
                          cluster_buffer)) {
            return false;
        }

        size_t bytes_in_cluster =
            static_cast<size_t>(entry.size - consumed);
        if (bytes_in_cluster > cluster_size) {
            bytes_in_cluster = cluster_size;
        }

        bool skip_cluster = false;
        size_t start = 0;
        if (offset > consumed) {
            if (offset >= consumed + bytes_in_cluster) {
                consumed += static_cast<uint32_t>(bytes_in_cluster);
                skip_cluster = true;
            } else {
                start = static_cast<size_t>(offset - consumed);
            }
        }

        if (!skip_cluster) {
            size_t available = bytes_in_cluster - start;
            if (available > buffer_size) {
                available = buffer_size;
            }

            if (available > 0) {
                memcpy(out_ptr, cluster_buffer + start, available);
                out_ptr += available;
                out_size += available;
                buffer_size -= available;
            }

            consumed += static_cast<uint32_t>(bytes_in_cluster);
            if (buffer_size == 0 || consumed >= entry.size) {
                return true;
            }
        } else if (consumed >= entry.size) {
            return true;
        }

        uint32_t next = read_fat_entry(volume, cluster);
        if (next >= kFatEoc) {
            return true;
        }
        if (next == kFatBadCluster) {
            log_message(LogLevel::Warn, "FAT32: bad cluster in chain");
            return false;
        }
        cluster = next;
    }

    return true;
}

bool fat32_write_file_range(Fat32Volume& volume,
                            Fat32DirEntry& entry,
                            uint32_t offset,
                            const void* buffer,
                            size_t buffer_size,
                            size_t& out_size) {
    out_size = 0;
    if (!volume.mounted || buffer == nullptr) {
        return false;
    }
    if ((entry.attributes & ATTR_DIRECTORY) != 0) {
        log_message(LogLevel::Warn, "FAT32: attempt to write directory");
        return false;
    }
    if (buffer_size == 0) {
        return true;
    }

    size_t cluster_size =
        static_cast<size_t>(volume.sectors_per_cluster) * 512u;
    if (cluster_size == 0) {
        return false;
    }
    if (cluster_size > sizeof(cluster_buffer)) {
        log_message(LogLevel::Warn,
                    "FAT32: cluster size %zu exceeds buffer capacity",
                    cluster_size);
        return false;
    }

    uint64_t end_offset64 =
        static_cast<uint64_t>(offset) + static_cast<uint64_t>(buffer_size);
    if (end_offset64 > UINT32_MAX) {
        log_message(LogLevel::Warn,
                    "FAT32: write exceeds maximum file size");
        return false;
    }
    uint32_t end_offset = static_cast<uint32_t>(end_offset64);

    uint32_t required_clusters =
        (end_offset == 0)
            ? 0
            : static_cast<uint32_t>(
                  (end_offset + static_cast<uint32_t>(cluster_size) - 1) /
                  static_cast<uint32_t>(cluster_size));

    if (required_clusters > 0) {
        if (!ensure_cluster_count(volume, entry, required_clusters)) {
            return false;
        }
    }

    if (offset > entry.size) {
        uint32_t gap = offset - entry.size;
        if (!zero_range(volume, entry, entry.size, gap)) {
            return false;
        }
    }

    const uint8_t* src = static_cast<const uint8_t*>(buffer);
    size_t remaining = buffer_size;
    uint32_t current_offset = offset;

    while (remaining > 0) {
        uint32_t cluster_index =
            current_offset / static_cast<uint32_t>(cluster_size);
        uint32_t cluster_offset =
            current_offset % static_cast<uint32_t>(cluster_size);
        uint32_t cluster_number = 0;
        if (!get_cluster_at_index(volume,
                                  entry.first_cluster,
                                  cluster_index,
                                  cluster_number)) {
            return false;
        }

        size_t chunk = cluster_size - cluster_offset;
        if (chunk > remaining) {
            chunk = remaining;
        }

        uint32_t lba = cluster_to_lba(volume, cluster_number);
        if (chunk == cluster_size && cluster_offset == 0) {
            if (!write_sectors(volume.device,
                               lba,
                               volume.sectors_per_cluster,
                               src)) {
                return false;
            }
        } else {
            if (!read_sectors(volume.device,
                              lba,
                              volume.sectors_per_cluster,
                              cluster_buffer)) {
                return false;
            }
            memcpy(cluster_buffer + cluster_offset, src, chunk);
            if (!write_sectors(volume.device,
                               lba,
                               volume.sectors_per_cluster,
                               cluster_buffer)) {
                return false;
            }
        }

        src += chunk;
        remaining -= chunk;
        current_offset += static_cast<uint32_t>(chunk);
        out_size += chunk;
    }

    if (end_offset > entry.size) {
        entry.size = end_offset;
    }

    return update_directory_entry(volume, entry);
}

bool fat32_get_entry_by_index(Fat32Volume& volume, uint32_t directory_cluster,
                              size_t index, Fat32DirEntry& out_entry) {
    if (!volume.mounted) {
        return false;
    }

    bool found = false;
    bool ok = iterate_directory(volume, directory_cluster,
                                [&](const uint8_t* raw,
                                    uint32_t dir_cluster,
                                    uint32_t raw_index,
                                    uint32_t visible_index) -> IterationResult {
        if (visible_index == index) {
            copy_entry(raw, dir_cluster, raw_index, out_entry);
            found = true;
            return IterationResult::StopSuccess;
        }
        return IterationResult::Continue;
    });

    return ok && found;
}

namespace {

constexpr size_t kMaxOpenFiles = 64;
constexpr size_t kMaxOpenDirectories = 32;

struct Fat32FileContext {
    Fat32Volume* volume;
    Fat32DirEntry entry;
};

struct Fat32DirectoryContext {
    Fat32Volume* volume;
    uint32_t cluster;
    uint32_t next_index;
};

Fat32FileContext g_file_contexts[kMaxOpenFiles];
bool g_file_context_used[kMaxOpenFiles];

Fat32DirectoryContext g_directory_contexts[kMaxOpenDirectories];
bool g_directory_context_used[kMaxOpenDirectories];

Fat32FileContext* allocate_file_context() {
    for (size_t i = 0; i < kMaxOpenFiles; ++i) {
        if (!g_file_context_used[i]) {
            g_file_context_used[i] = true;
            g_file_contexts[i].volume = nullptr;
            memset(&g_file_contexts[i].entry, 0, sizeof(Fat32DirEntry));
            return &g_file_contexts[i];
        }
    }
    return nullptr;
}

void release_file_context(Fat32FileContext* ctx) {
    if (ctx == nullptr) {
        return;
    }
    size_t index = static_cast<size_t>(ctx - g_file_contexts);
    if (index < kMaxOpenFiles) {
        g_file_context_used[index] = false;
    }
}

Fat32DirectoryContext* allocate_directory_context() {
    for (size_t i = 0; i < kMaxOpenDirectories; ++i) {
        if (!g_directory_context_used[i]) {
            g_directory_context_used[i] = true;
            g_directory_contexts[i].volume = nullptr;
            g_directory_contexts[i].cluster = 0;
            g_directory_contexts[i].next_index = 0;
            return &g_directory_contexts[i];
        }
    }
    return nullptr;
}

void release_directory_context(Fat32DirectoryContext* ctx) {
    if (ctx == nullptr) {
        return;
    }
    size_t index = static_cast<size_t>(ctx - g_directory_contexts);
    if (index < kMaxOpenDirectories) {
        g_directory_context_used[index] = false;
    }
}

bool fat32_vfs_list_directory(void* fs_context,
                              const char* path,
                              vfs::DirEntry* entries,
                              size_t max_entries,
                              size_t& out_count) {
    out_count = 0;
    if (fs_context == nullptr || entries == nullptr || max_entries == 0) {
        return false;
    }

    auto* volume = static_cast<Fat32Volume*>(fs_context);
    uint32_t cluster = 0;
    if (!resolve_directory_cluster(*volume, path, cluster)) {
        return false;
    }

    size_t collected = 0;
    while (collected < max_entries) {
        Fat32DirEntry entry{};
        if (!fat32_get_entry_by_index(*volume, cluster, collected, entry)) {
            break;
        }
        to_vfs_entry(entry, entries[collected]);
        ++collected;
    }

    out_count = collected;
    return true;
}

bool fat32_vfs_open_file(void* fs_context,
                         const char* path,
                         void*& out_file_context,
                         vfs::DirEntry* out_metadata) {
    if (fs_context == nullptr || path == nullptr || *path == '\0') {
        return false;
    }

    auto* volume = static_cast<Fat32Volume*>(fs_context);
    Fat32DirEntry entry{};
    if (!resolve_entry(*volume, path, entry)) {
        return false;
    }
    if ((entry.attributes & ATTR_DIRECTORY) != 0) {
        return false;
    }

    Fat32FileContext* ctx = allocate_file_context();
    if (ctx == nullptr) {
        log_message(LogLevel::Warn, "FAT32: out of file contexts");
        return false;
    }
    ctx->volume = volume;
    ctx->entry = entry;
    out_file_context = ctx;

    if (out_metadata != nullptr) {
        to_vfs_entry(entry, *out_metadata);
    }
    return true;
}

bool fat32_vfs_create_file(void* fs_context,
                           const char* path,
                           void*& out_file_context,
                           vfs::DirEntry* out_metadata) {
    if (fs_context == nullptr || path == nullptr || *path == '\0') {
        return false;
    }

    auto* volume = static_cast<Fat32Volume*>(fs_context);
    Fat32DirEntry entry{};
    if (!fat32_create_file(*volume, path, entry)) {
        return false;
    }

    Fat32FileContext* ctx = allocate_file_context();
    if (ctx == nullptr) {
        log_message(LogLevel::Warn, "FAT32: out of file contexts");
        return false;
    }

    ctx->volume = volume;
    ctx->entry = entry;
    out_file_context = ctx;

    if (out_metadata != nullptr) {
        to_vfs_entry(entry, *out_metadata);
    }
    return true;
}

bool fat32_vfs_read_file(void* file_context,
                         uint64_t offset,
                         void* buffer,
                         size_t buffer_size,
                         size_t& out_size) {
    out_size = 0;
    if (file_context == nullptr || buffer == nullptr) {
        return false;
    }

    auto* ctx = static_cast<Fat32FileContext*>(file_context);
    if (offset > 0xFFFFFFFFull) {
        return true;
    }

    uint32_t offset32 = static_cast<uint32_t>(offset);
    if (offset32 >= ctx->entry.size) {
        return true;
    }

    return fat32_read_file_range(*ctx->volume,
                                 ctx->entry,
                                 offset32,
                                 buffer,
                                 buffer_size,
                                 out_size);
}

bool fat32_vfs_write_file(void* file_context,
                          uint64_t offset,
                          const void* buffer,
                          size_t buffer_size,
                          size_t& out_size) {
    out_size = 0;
    if (file_context == nullptr || buffer == nullptr) {
        return false;
    }

    auto* ctx = static_cast<Fat32FileContext*>(file_context);
    if (offset > 0xFFFFFFFFull) {
        return true;
    }

    uint32_t offset32 = static_cast<uint32_t>(offset);
    return fat32_write_file_range(*ctx->volume,
                                  ctx->entry,
                                  offset32,
                                  buffer,
                                  buffer_size,
                                  out_size);
}

void fat32_vfs_close_file(void* file_context) {
    auto* ctx = static_cast<Fat32FileContext*>(file_context);
    release_file_context(ctx);
}

bool fat32_vfs_open_directory(void* fs_context,
                              const char* path,
                              void*& out_dir_context) {
    if (fs_context == nullptr) {
        return false;
    }

    auto* volume = static_cast<Fat32Volume*>(fs_context);
    uint32_t cluster = 0;
    if (!resolve_directory_cluster(*volume, path, cluster)) {
        return false;
    }

    Fat32DirectoryContext* ctx = allocate_directory_context();
    if (ctx == nullptr) {
        log_message(LogLevel::Warn, "FAT32: out of directory contexts");
        return false;
    }

    ctx->volume = volume;
    ctx->cluster = cluster;
    ctx->next_index = 0;
    out_dir_context = ctx;
    return true;
}

bool fat32_vfs_directory_next(void* dir_context, vfs::DirEntry& out_entry) {
    if (dir_context == nullptr) {
        return false;
    }

    auto* ctx = static_cast<Fat32DirectoryContext*>(dir_context);
    Fat32DirEntry entry{};
    if (!fat32_get_entry_by_index(*ctx->volume,
                                  ctx->cluster,
                                  ctx->next_index,
                                  entry)) {
        return false;
    }

    ++ctx->next_index;
    to_vfs_entry(entry, out_entry);
    return true;
}

void fat32_vfs_close_directory(void* dir_context) {
    auto* ctx = static_cast<Fat32DirectoryContext*>(dir_context);
    release_directory_context(ctx);
}

const vfs::FilesystemOps kFat32FilesystemOps{
    &fat32_vfs_list_directory,
    &fat32_vfs_open_file,
    &fat32_vfs_create_file,
    &fat32_vfs_read_file,
    &fat32_vfs_write_file,
    &fat32_vfs_close_file,
    &fat32_vfs_open_directory,
    &fat32_vfs_directory_next,
    &fat32_vfs_close_directory,
};

}  // namespace

const vfs::FilesystemOps& fat32_vfs_ops() {
    return kFat32FilesystemOps;
}
