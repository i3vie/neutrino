#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "descriptors.hpp"
#include "../crt/syscall.hpp"
#include "../helpers/args.hpp"
#include "../helpers/console.hpp"

namespace {

constexpr uint32_t kDescConsole =
    static_cast<uint32_t>(descriptor_defs::Type::Console);
constexpr uint32_t kDescBlock =
    static_cast<uint32_t>(descriptor_defs::Type::BlockDevice);
constexpr uint32_t kDescDisk =
    static_cast<uint32_t>(descriptor_defs::Type::Disk);
constexpr uint32_t kDescPartition =
    static_cast<uint32_t>(descriptor_defs::Type::Partition);

struct Args {
    char first[64];
    char second[64];
    char third[64];
    uint32_t count;
};

bool parse_args(const char* raw, Args& out) {
    out = {};
    const char* cursor = raw;
    if (!userspace::copy_token(cursor, out.first, sizeof(out.first))) {
        return false;
    }
    out.count = 1;
    if (userspace::copy_token(cursor, out.second, sizeof(out.second))) {
        out.count = 2;
    }
    if (userspace::copy_token(cursor, out.third, sizeof(out.third))) {
        out.count = 3;
    }
    cursor = userspace::skip_spaces(cursor);
    return cursor == nullptr || *cursor == '\0';
}

bool parse_u32(const char* text, uint32_t& out) {
    if (text == nullptr || text[0] == '\0') {
        return false;
    }
    uint64_t value = 0;
    for (size_t i = 0; text[i] != '\0'; ++i) {
        if (text[i] < '0' || text[i] > '9') {
            return false;
        }
        value = value * 10 + static_cast<uint64_t>(text[i] - '0');
        if (value > 0xFFFFFFFFull) {
            return false;
        }
    }
    out = static_cast<uint32_t>(value);
    return true;
}

void usage(long console) {
    userspace::write_line(console, "usage: mount <disk> [partition-index] [mount-name]");
    userspace::write_line(console, "       mount <partition> [mount-name]");
    userspace::write_line(console, "examples: mount AHCI_0 0 data");
    userspace::write_line(console, "          mount AHCI_0_0 data");
}

void print_partition(long console,
                     const descriptor_defs::PartitionInfo& info) {
    userspace::write(console, "  ");
    userspace::write_u64(console, info.index);
    userspace::write(console, ": ");
    userspace::write(console, info.name);
    userspace::write(console, " sectors=");
    userspace::write_u64(console, info.sector_count);
    userspace::write_line(console, "");
}

long open_partition_from_disk(long console,
                              const char* disk_name,
                              uint32_t partition_index) {
    long disk = descriptor_open(
        kDescDisk,
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(disk_name)),
        0,
        0);
    if (disk < 0) {
        userspace::write(console, "mount: unable to open disk ");
        userspace::write_line(console, disk_name);
        return -1;
    }

    descriptor_defs::DiskInfo disk_info{};
    if (descriptor_get_property(
            static_cast<uint32_t>(disk),
            static_cast<uint32_t>(descriptor_defs::Property::DiskInfo),
            &disk_info,
            sizeof(disk_info)) != 0) {
        userspace::write_line(console, "mount: unable to query disk");
        descriptor_close(static_cast<uint32_t>(disk));
        return -1;
    }

    if (partition_index >= disk_info.partition_count) {
        userspace::write(console, "mount: partition index out of range; ");
        userspace::write(console, disk_info.name);
        userspace::write_line(console, " has:");
        for (uint32_t i = 0; i < disk_info.partition_count; ++i) {
            descriptor_defs::PartitionInfo info{};
            if (descriptor_read(static_cast<uint32_t>(disk),
                                &info,
                                sizeof(info),
                                static_cast<uint64_t>(i) * sizeof(info)) ==
                static_cast<long>(sizeof(info))) {
                print_partition(console, info);
            }
        }
        descriptor_close(static_cast<uint32_t>(disk));
        return -1;
    }

    long partition = descriptor_open(
        kDescPartition,
        static_cast<uint64_t>(disk),
        partition_index,
        0);
    descriptor_close(static_cast<uint32_t>(disk));
    if (partition < 0) {
        userspace::write_line(console, "mount: unable to open partition");
        return -1;
    }
    return partition;
}

bool mount_handle(long console, long handle, const char* mount_name) {
    if (mount_descriptor(static_cast<uint32_t>(handle), mount_name) != 0) {
        userspace::write_line(console, "mount: mount syscall failed");
        return false;
    }
    return true;
}

}  // namespace

int main(uint64_t arg_ptr, uint64_t) {
    long console = process_get_standard_descriptor(1);
    if (console < 0) {
        console = descriptor_open(kDescConsole, 0);
    }

    Args args{};
    if (!parse_args(reinterpret_cast<const char*>(arg_ptr), args)) {
        usage(console);
        return 1;
    }

    uint32_t partition_index = 0;
    const char* mount_name = nullptr;
    long partition = -1;

    if (args.count >= 2 && parse_u32(args.second, partition_index)) {
        mount_name = (args.count >= 3) ? args.third : nullptr;
        partition = open_partition_from_disk(console, args.first, partition_index);
    } else {
        mount_name = (args.count >= 2) ? args.second : nullptr;
        partition = descriptor_open(
            kDescBlock,
            static_cast<uint64_t>(reinterpret_cast<uintptr_t>(args.first)),
            0,
            0);
        if (partition < 0) {
            partition = open_partition_from_disk(console, args.first, 0);
        }
    }

    if (partition < 0) {
        return 1;
    }

    bool ok = mount_handle(console, partition, mount_name);
    if (ok) {
        userspace::write(console, "mount: mounted ");
        userspace::write(console, args.first);
        if (mount_name != nullptr && mount_name[0] != '\0') {
            userspace::write(console, " as ");
            userspace::write(console, mount_name);
        }
        userspace::write_line(console, "");
    }
    descriptor_close(static_cast<uint32_t>(partition));
    return ok ? 0 : 1;
}
