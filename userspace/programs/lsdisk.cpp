#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "descriptors.hpp"
#include "../crt/syscall.hpp"

namespace {

constexpr uint32_t kDescConsole =
    static_cast<uint32_t>(descriptor_defs::Type::Console);
constexpr uint32_t kDescDisk =
    static_cast<uint32_t>(descriptor_defs::Type::Disk);
constexpr uint32_t kDescPartition =
    static_cast<uint32_t>(descriptor_defs::Type::Partition);

constexpr uint8_t kNeufsMagic[8] = {
    0x4E, 0x45, 0x55, 0x46, 0x53, 0x00, 0x77, 0x42};

void print(long console, const char* text) {
    if (console < 0 || text == nullptr) {
        return;
    }
    descriptor_write(static_cast<uint32_t>(console), text, strlen(text));
}

void print_line(long console, const char* text) {
    print(console, text);
    descriptor_write(static_cast<uint32_t>(console), "\n", 1);
}

void print_u64(long console, uint64_t value) {
    char buffer[32];
    size_t pos = sizeof(buffer);
    buffer[--pos] = '\0';
    if (value == 0) {
        buffer[--pos] = '0';
    } else {
        while (value != 0 && pos > 0) {
            buffer[--pos] = static_cast<char>('0' + (value % 10));
            value /= 10;
        }
    }
    print(console, &buffer[pos]);
}

void print_size(long console, uint64_t bytes) {
    constexpr uint64_t kib = 1024ull;
    constexpr uint64_t mib = kib * 1024ull;
    constexpr uint64_t gib = mib * 1024ull;
    if (bytes >= gib) {
        print_u64(console, bytes / gib);
        print(console, ".");
        print_u64(console, ((bytes % gib) * 10) / gib);
        print(console, " GiB");
    } else if (bytes >= mib) {
        print_u64(console, bytes / mib);
        print(console, ".");
        print_u64(console, ((bytes % mib) * 10) / mib);
        print(console, " MiB");
    } else if (bytes >= kib) {
        print_u64(console, bytes / kib);
        print(console, ".");
        print_u64(console, ((bytes % kib) * 10) / kib);
        print(console, " KiB");
    } else {
        print_u64(console, bytes);
        print(console, " B");
    }
}

uint16_t read_u16_le(const uint8_t* data) {
    return static_cast<uint16_t>(data[0]) |
           static_cast<uint16_t>(data[1] << 8);
}

uint32_t read_u32_le(const uint8_t* data) {
    return static_cast<uint32_t>(data[0]) |
           (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) |
           (static_cast<uint32_t>(data[3]) << 24);
}

bool bytes_equal(const uint8_t* a, const uint8_t* b, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (a[i] != b[i]) {
            return false;
        }
    }
    return true;
}

const char* detect_fs(uint32_t partition_handle,
                      const descriptor_defs::BlockGeometry& geom,
                      uint8_t* sector,
                      size_t sector_capacity) {
    if (geom.sector_size == 0 || geom.sector_size > sector_capacity) {
        return "unknown";
    }
    long read = descriptor_read(partition_handle,
                                sector,
                                static_cast<size_t>(geom.sector_size),
                                0);
    if (read != static_cast<long>(geom.sector_size)) {
        return "unreadable";
    }

    if (geom.sector_size >= sizeof(kNeufsMagic) &&
        bytes_equal(sector, kNeufsMagic, sizeof(kNeufsMagic))) {
        return "neufs";
    }

    if (geom.sector_size >= 512 &&
        sector[510] == 0x55 &&
        sector[511] == 0xAA &&
        read_u16_le(sector + 11) == 512 &&
        sector[13] != 0 &&
        read_u32_le(sector + 36) != 0) {
        return "fat32";
    }

    return "unknown";
}

void print_partition_type(long console, uint8_t type) {
    if (type == 0xFF) {
        print(console, "whole");
        return;
    }
    const char hex[] = "0123456789ABCDEF";
    char buffer[5];
    buffer[0] = '0';
    buffer[1] = 'x';
    buffer[2] = hex[(type >> 4) & 0x0F];
    buffer[3] = hex[type & 0x0F];
    buffer[4] = '\0';
    print(console, buffer);
}

bool list_disk(long console, uint64_t disk_index) {
    long disk = descriptor_open(kDescDisk, 0, disk_index, 0);
    if (disk < 0) {
        return false;
    }

    descriptor_defs::DiskInfo disk_info{};
    if (descriptor_get_property(
            static_cast<uint32_t>(disk),
            static_cast<uint32_t>(descriptor_defs::Property::DiskInfo),
            &disk_info,
            sizeof(disk_info)) != 0) {
        descriptor_close(static_cast<uint32_t>(disk));
        return false;
    }

    print(console, "Disk ");
    print(console, disk_info.name);
    print(console, " (");
    print(console,
          (disk_info.flags & descriptor_defs::kDiskFlagRemovable) != 0
              ? "removable"
              : "fixed");
    print(console, "): ");
    print_u64(console, disk_info.partition_count);
    print_line(console, " partition(s)");

    for (uint32_t i = 0; i < disk_info.partition_count; ++i) {
        descriptor_defs::PartitionInfo part_info{};
        long part_read = descriptor_read(
            static_cast<uint32_t>(disk),
            &part_info,
            sizeof(part_info),
            static_cast<uint64_t>(i) * sizeof(part_info));
        if (part_read != static_cast<long>(sizeof(part_info))) {
            continue;
        }

        long part = descriptor_open(
            kDescPartition,
            static_cast<uint64_t>(disk),
            i,
            0);
        descriptor_defs::BlockGeometry geom{};
        const char* fs_type = "unknown";
        uint64_t sector_size = 512;
        if (part >= 0 &&
            descriptor_get_property(
                static_cast<uint32_t>(part),
                static_cast<uint32_t>(
                    descriptor_defs::Property::BlockGeometry),
                &geom,
                sizeof(geom)) == 0) {
            sector_size = geom.sector_size;
            uint8_t sector[4096];
            fs_type = detect_fs(static_cast<uint32_t>(part),
                                geom,
                                sector,
                                sizeof(sector));
        }

        print(console, "  ");
        print(console, part_info.name);
        print(console, " start=");
        print_u64(console, part_info.start_lba);
        print(console, " sectors=");
        print_u64(console, part_info.sector_count);
        print(console, " size=");
        print_size(console, part_info.sector_count * sector_size);
        print(console, " type=");
        print_partition_type(console, part_info.type);
        print(console, " fs=");
        print_line(console, fs_type);

        if (part >= 0) {
            descriptor_close(static_cast<uint32_t>(part));
        }
    }

    descriptor_close(static_cast<uint32_t>(disk));
    print_line(console, "");
    return true;
}

bool valid_args(const char* raw) {
    raw = (raw == nullptr) ? "" : raw;
    while (*raw == ' ' || *raw == '\t' || *raw == '\n' || *raw == '\r') {
        ++raw;
    }
    if (*raw == '\0') {
        return true;
    }
    if (raw[0] == '-' && raw[1] == 'l' && raw[2] == '\0') {
        return true;
    }
    return false;
}

}  // namespace

int main(uint64_t arg_ptr, uint64_t) {
    long console = process_get_standard_descriptor(1);
    if (console < 0) {
        console = descriptor_open(kDescConsole, 0);
    }

    if (!valid_args(reinterpret_cast<const char*>(arg_ptr))) {
        print_line(console, "usage: lsdisk [-l]");
        return 1;
    }

    bool any = false;
    for (uint64_t index = 0;; ++index) {
        if (!list_disk(console, index)) {
            break;
        }
        any = true;
    }

    if (!any) {
        print_line(console, "lsdisk: no disks found");
        return 1;
    }
    return 0;
}
