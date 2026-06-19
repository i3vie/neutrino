#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "descriptors.hpp"
#include "../crt/syscall.hpp"

namespace {

constexpr uint32_t kDescConsole =
    static_cast<uint32_t>(descriptor_defs::Type::Console);
constexpr uint32_t kDescBlock =
    static_cast<uint32_t>(descriptor_defs::Type::BlockDevice);
constexpr long kWouldBlock = -2;
constexpr uint64_t kDefaultStartLba = 2048;
constexpr uint8_t kDefaultMbrType = 0x83;
constexpr size_t kGptEntryCount = 128;
constexpr size_t kGptEntrySize = 128;

constexpr uint8_t kNeufsMagic[8] = {
    0x4E, 0x45, 0x55, 0x46, 0x53, 0x00, 0x77, 0x42};

enum class TableKind {
    Mbr,
    Gpt,
};

struct Args {
    char device[64];
    TableKind table;
    uint8_t mbr_type;
    uint64_t start_lba;
    uint64_t sector_count;
    bool has_count;
    bool wipe;
};

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

void print_hex8(long console, uint8_t value) {
    const char* digits = "0123456789abcdef";
    char buffer[5];
    buffer[0] = '0';
    buffer[1] = 'x';
    buffer[2] = digits[(value >> 4) & 0x0F];
    buffer[3] = digits[value & 0x0F];
    buffer[4] = '\0';
    print(console, buffer);
}

const char* skip_spaces(const char* text) {
    while (text != nullptr && (*text == ' ' || *text == '\t' ||
                               *text == '\n' || *text == '\r')) {
        ++text;
    }
    return text;
}

bool copy_token(const char*& cursor, char* out, size_t out_size) {
    if (out == nullptr || out_size == 0) {
        return false;
    }
    cursor = skip_spaces(cursor);
    if (cursor == nullptr || *cursor == '\0') {
        return false;
    }

    size_t len = 0;
    while (cursor[len] != '\0' && cursor[len] != ' ' &&
           cursor[len] != '\t' && cursor[len] != '\n' &&
           cursor[len] != '\r') {
        if (len + 1 >= out_size) {
            return false;
        }
        out[len] = cursor[len];
        ++len;
    }
    out[len] = '\0';
    cursor += len;
    return true;
}

bool strings_equal(const char* a, const char* b) {
    if (a == nullptr || b == nullptr) {
        return false;
    }
    while (*a != '\0' && *b != '\0') {
        if (*a != *b) {
            return false;
        }
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

uint8_t hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return static_cast<uint8_t>(c - '0');
    }
    if (c >= 'a' && c <= 'f') {
        return static_cast<uint8_t>(10 + c - 'a');
    }
    if (c >= 'A' && c <= 'F') {
        return static_cast<uint8_t>(10 + c - 'A');
    }
    return 0xFF;
}

bool parse_u64(const char* text, uint64_t& out) {
    text = skip_spaces(text);
    if (text == nullptr || *text == '\0') {
        return false;
    }

    uint32_t base = 10;
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16;
        text += 2;
    }
    if (*text == '\0') {
        return false;
    }

    uint64_t value = 0;
    while (*text != '\0') {
        uint8_t digit = (base == 16) ? hex_value(*text)
                                     : static_cast<uint8_t>(*text - '0');
        if (digit >= base) {
            return false;
        }
        value = value * base + digit;
        ++text;
    }
    out = value;
    return true;
}

bool parse_value_option(const char*& cursor, uint64_t& out) {
    char token[32];
    if (!copy_token(cursor, token, sizeof(token))) {
        return false;
    }
    return parse_u64(token, out);
}

bool parse_args(const char* raw, Args& out) {
    out = {};
    out.table = TableKind::Gpt;
    out.mbr_type = kDefaultMbrType;
    out.start_lba = kDefaultStartLba;

    const char* cursor = raw;
    char positional[3][32]{};
    size_t positional_count = 0;

    char token[64];
    while (copy_token(cursor, token, sizeof(token))) {
        if (strings_equal(token, "--mbr")) {
            out.table = TableKind::Mbr;
        } else if (strings_equal(token, "--gpt")) {
            out.table = TableKind::Gpt;
        } else if (strings_equal(token, "--wipe")) {
            out.wipe = true;
        } else if (strings_equal(token, "--type")) {
            uint64_t value = 0;
            if (!parse_value_option(cursor, value) || value == 0 || value > 0xFF) {
                return false;
            }
            out.mbr_type = static_cast<uint8_t>(value);
        } else if (strings_equal(token, "--start")) {
            if (!parse_value_option(cursor, out.start_lba)) {
                return false;
            }
        } else if (strings_equal(token, "--sectors")) {
            if (!parse_value_option(cursor, out.sector_count) ||
                out.sector_count == 0) {
                return false;
            }
            out.has_count = true;
        } else if (out.device[0] == '\0') {
            memcpy(out.device, token, strlen(token) + 1);
        } else {
            if (positional_count >= 3) {
                return false;
            }
            memcpy(positional[positional_count],
                   token,
                   strlen(token) + 1);
            ++positional_count;
        }
    }

    if (out.device[0] == '\0') {
        return false;
    }

    if (positional_count > 0) {
        uint64_t value = 0;
        if (!parse_u64(positional[0], value) || value == 0 || value > 0xFF) {
            return false;
        }
        out.mbr_type = static_cast<uint8_t>(value);
    }
    if (positional_count > 1) {
        if (!parse_u64(positional[1], out.start_lba)) {
            return false;
        }
    }
    if (positional_count > 2) {
        if (!parse_u64(positional[2], out.sector_count) ||
            out.sector_count == 0) {
            return false;
        }
        out.has_count = true;
    }

    return true;
}

long read_with_retry(uint32_t handle, void* buffer, size_t length, uint64_t offset) {
    while (true) {
        long result = descriptor_read(handle, buffer, length, offset);
        if (result == kWouldBlock) {
            yield();
            continue;
        }
        return result;
    }
}

long write_with_retry(uint32_t handle,
                      const void* buffer,
                      size_t length,
                      uint64_t offset) {
    while (true) {
        long result = descriptor_write(handle, buffer, length, offset);
        if (result == kWouldBlock) {
            yield();
            continue;
        }
        return result;
    }
}

uint32_t read_u32_le(const uint8_t* data) {
    return static_cast<uint32_t>(data[0]) |
           (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) |
           (static_cast<uint32_t>(data[3]) << 24);
}

void write_u32_le(uint8_t* out, uint32_t value) {
    out[0] = static_cast<uint8_t>(value & 0xFF);
    out[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
    out[2] = static_cast<uint8_t>((value >> 16) & 0xFF);
    out[3] = static_cast<uint8_t>((value >> 24) & 0xFF);
}

void write_u64_le(uint8_t* out, uint64_t value) {
    write_u32_le(out, static_cast<uint32_t>(value & 0xFFFFFFFFull));
    write_u32_le(out + 4, static_cast<uint32_t>(value >> 32));
}

uint64_t align_up(uint64_t value, uint64_t alignment) {
    uint64_t rem = value % alignment;
    return rem == 0 ? value : value + (alignment - rem);
}

uint32_t crc32(const uint8_t* data, size_t length) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            uint32_t mask = static_cast<uint32_t>(-(crc & 1u));
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

bool existing_layout(uint32_t handle,
                     const descriptor_defs::BlockGeometry& geom,
                     uint8_t* sector,
                     const char*& reason) {
    reason = nullptr;
    if (geom.sector_size < 512 || geom.sector_size > 4096) {
        reason = "unsupported sector size";
        return true;
    }
    if (read_with_retry(handle,
                        sector,
                        static_cast<size_t>(geom.sector_size),
                        0) != static_cast<long>(geom.sector_size)) {
        reason = "unable to read sector 0";
        return true;
    }
    if (memcmp(sector, kNeufsMagic, sizeof(kNeufsMagic)) == 0) {
        reason = "whole-disk NEUFS";
        return true;
    }
    if (sector[510] == 0x55 && sector[511] == 0xAA) {
        for (size_t i = 0; i < 4; ++i) {
            const uint8_t* entry = sector + 446 + i * 16;
            if (entry[4] != 0 && read_u32_le(entry + 12) != 0) {
                reason = "existing MBR";
                return true;
            }
        }
    }
    if (geom.sector_count > 1 &&
        read_with_retry(handle,
                        sector,
                        static_cast<size_t>(geom.sector_size),
                        geom.sector_size) == static_cast<long>(geom.sector_size)) {
        const uint8_t signature[8] = {'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T'};
        if (memcmp(sector, signature, sizeof(signature)) == 0) {
            reason = "existing GPT";
            return true;
        }
    }
    return false;
}

bool zero_lba(uint32_t handle,
              const descriptor_defs::BlockGeometry& geom,
              uint64_t lba,
              uint8_t* sector) {
    memset(sector, 0, static_cast<size_t>(geom.sector_size));
    return write_with_retry(handle,
                            sector,
                            static_cast<size_t>(geom.sector_size),
                            lba * geom.sector_size) ==
           static_cast<long>(geom.sector_size);
}

bool wipe_partition_metadata(uint32_t handle,
                             const descriptor_defs::BlockGeometry& geom,
                             uint8_t* sector) {
    uint64_t front = geom.sector_count < 34 ? geom.sector_count : 34;
    for (uint64_t lba = 0; lba < front; ++lba) {
        if (!zero_lba(handle, geom, lba, sector)) {
            return false;
        }
    }
    if (geom.sector_count > 34) {
        uint64_t start = geom.sector_count - 33;
        for (uint64_t lba = start; lba < geom.sector_count; ++lba) {
            if (!zero_lba(handle, geom, lba, sector)) {
                return false;
            }
        }
    }
    return true;
}

bool prepare_overwrite(long console,
                       uint32_t handle,
                       const Args& args,
                       const descriptor_defs::BlockGeometry& geom,
                       uint8_t* sector) {
    const char* reason = nullptr;
    if (existing_layout(handle, geom, sector, reason) && !args.wipe) {
        print(console, "mkpart: refusing to overwrite ");
        print_line(console, reason != nullptr ? reason : "existing data");
        print_line(console, "mkpart: pass --wipe to replace partition metadata");
        return false;
    }
    if (args.wipe && !wipe_partition_metadata(handle, geom, sector)) {
        print_line(console, "mkpart: failed to wipe partition metadata");
        return false;
    }
    return true;
}

bool partition_bounds(long console,
                      const Args& args,
                      const descriptor_defs::BlockGeometry& geom,
                      uint64_t first_allowed,
                      uint64_t last_allowed,
                      uint64_t& start_lba,
                      uint64_t& sector_count) {
    start_lba = args.start_lba;
    if (start_lba < first_allowed) {
        start_lba = first_allowed;
    }
    if (last_allowed <= start_lba) {
        print_line(console, "mkpart: disk too small for requested layout");
        return false;
    }

    sector_count = args.has_count ? args.sector_count : last_allowed - start_lba + 1;
    if (sector_count == 0 || start_lba + sector_count - 1 > last_allowed ||
        start_lba + sector_count > geom.sector_count) {
        print_line(console, "mkpart: partition does not fit");
        return false;
    }
    return true;
}

bool write_mbr(long console,
               uint32_t handle,
               const Args& args,
               const descriptor_defs::BlockGeometry& geom,
               uint8_t* sector) {
    uint64_t start_lba = 0;
    uint64_t sector_count = 0;
    if (!partition_bounds(console,
                          args,
                          geom,
                          1,
                          geom.sector_count - 1,
                          start_lba,
                          sector_count)) {
        return false;
    }
    if (start_lba > 0xFFFFFFFFull || sector_count > 0xFFFFFFFFull) {
        print_line(console, "mkpart: partition does not fit in MBR limits");
        return false;
    }

    memset(sector, 0, static_cast<size_t>(geom.sector_size));
    uint8_t* entry = sector + 446;
    entry[1] = 0xFF;
    entry[2] = 0xFF;
    entry[3] = 0xFF;
    entry[4] = args.mbr_type;
    entry[5] = 0xFF;
    entry[6] = 0xFF;
    entry[7] = 0xFF;
    write_u32_le(entry + 8, static_cast<uint32_t>(start_lba));
    write_u32_le(entry + 12, static_cast<uint32_t>(sector_count));
    sector[510] = 0x55;
    sector[511] = 0xAA;

    if (write_with_retry(handle,
                         sector,
                         static_cast<size_t>(geom.sector_size),
                         0) != static_cast<long>(geom.sector_size)) {
        print_line(console, "mkpart: failed to write MBR");
        return false;
    }

    print(console, "mkpart: created MBR ");
    print(console, args.device);
    print(console, "_0 type=");
    print_hex8(console, args.mbr_type);
    print(console, " start=");
    print_u64(console, start_lba);
    print(console, " sectors=");
    print_u64(console, sector_count);
    print_line(console, "");
    return true;
}

void write_utf16_name(uint8_t* entry, const char* name) {
    uint8_t* out = entry + 56;
    for (size_t i = 0; i < 36; ++i) {
        uint16_t ch = 0;
        if (name != nullptr && name[i] != '\0') {
            ch = static_cast<uint8_t>(name[i]);
        }
        out[i * 2] = static_cast<uint8_t>(ch & 0xFF);
        out[i * 2 + 1] = static_cast<uint8_t>(ch >> 8);
        if (ch == 0) {
            break;
        }
    }
}

void write_gpt_entry(uint8_t* table,
                     uint64_t first_lba,
                     uint64_t last_lba,
                     const char* name) {
    const uint8_t linux_type[16] = {
        0xAF, 0x3D, 0xC6, 0x0F, 0x83, 0x84, 0x72, 0x47,
        0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4};
    const uint8_t unique_guid[16] = {
        0x4E, 0x54, 0x52, 0x4E, 0x50, 0x41, 0x52, 0x54,
        0x90, 0x01, 0x12, 0x34, 0x56, 0x78, 0x00, 0x01};
    memcpy(table, linux_type, 16);
    memcpy(table + 16, unique_guid, 16);
    write_u64_le(table + 32, first_lba);
    write_u64_le(table + 40, last_lba);
    write_u64_le(table + 48, 0);
    write_utf16_name(table, name);
}

void write_gpt_header(uint8_t* sector,
                      uint64_t current_lba,
                      uint64_t backup_lba,
                      uint64_t first_usable_lba,
                      uint64_t last_usable_lba,
                      uint64_t entries_lba,
                      uint32_t entries_crc,
                      const uint8_t disk_guid[16]) {
    memset(sector, 0, 512);
    const uint8_t signature[8] = {'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T'};
    memcpy(sector, signature, sizeof(signature));
    write_u32_le(sector + 8, 0x00010000u);
    write_u32_le(sector + 12, 92);
    write_u64_le(sector + 24, current_lba);
    write_u64_le(sector + 32, backup_lba);
    write_u64_le(sector + 40, first_usable_lba);
    write_u64_le(sector + 48, last_usable_lba);
    memcpy(sector + 56, disk_guid, 16);
    write_u64_le(sector + 72, entries_lba);
    write_u32_le(sector + 80, static_cast<uint32_t>(kGptEntryCount));
    write_u32_le(sector + 84, static_cast<uint32_t>(kGptEntrySize));
    write_u32_le(sector + 88, entries_crc);
    write_u32_le(sector + 16, crc32(sector, 92));
}

bool write_gpt(long console,
               uint32_t handle,
               const Args& args,
               const descriptor_defs::BlockGeometry& geom,
               uint8_t* sector) {
    uint64_t entries_bytes = kGptEntryCount * kGptEntrySize;
    uint64_t entry_sectors = align_up(entries_bytes, geom.sector_size) /
                             geom.sector_size;
    uint64_t primary_entries_lba = 2;
    uint64_t first_usable_lba = primary_entries_lba + entry_sectors;
    uint64_t backup_header_lba = geom.sector_count - 1;
    uint64_t backup_entries_lba = backup_header_lba - entry_sectors;
    uint64_t last_usable_lba = backup_entries_lba - 1;
    uint64_t start_lba = 0;
    uint64_t sector_count = 0;
    if (!partition_bounds(console,
                          args,
                          geom,
                          first_usable_lba,
                          last_usable_lba,
                          start_lba,
                          sector_count)) {
        return false;
    }
    uint64_t last_lba = start_lba + sector_count - 1;

    memset(sector, 0, static_cast<size_t>(geom.sector_size));
    sector[446 + 4] = 0xEE;
    write_u32_le(sector + 446 + 8, 1);
    uint64_t protected_sectors = geom.sector_count - 1;
    if (protected_sectors > 0xFFFFFFFFull) {
        protected_sectors = 0xFFFFFFFFull;
    }
    write_u32_le(sector + 446 + 12, static_cast<uint32_t>(protected_sectors));
    sector[510] = 0x55;
    sector[511] = 0xAA;
    if (write_with_retry(handle,
                         sector,
                         static_cast<size_t>(geom.sector_size),
                         0) != static_cast<long>(geom.sector_size)) {
        print_line(console, "mkpart: failed to write protective MBR");
        return false;
    }

    uint8_t* entries = static_cast<uint8_t*>(
        map_anonymous(static_cast<size_t>(entries_bytes), MAP_WRITE));
    if (entries == nullptr) {
        print_line(console, "mkpart: allocation failed");
        return false;
    }
    memset(entries, 0, static_cast<size_t>(entries_bytes));
    write_gpt_entry(entries, start_lba, last_lba, "Neutrino Data");
    uint32_t entries_crc = crc32(entries, static_cast<size_t>(entries_bytes));

    if (write_with_retry(handle,
                         entries,
                         static_cast<size_t>(entries_bytes),
                         primary_entries_lba * geom.sector_size) !=
        static_cast<long>(entries_bytes) ||
        write_with_retry(handle,
                         entries,
                         static_cast<size_t>(entries_bytes),
                         backup_entries_lba * geom.sector_size) !=
        static_cast<long>(entries_bytes)) {
        unmap(entries, static_cast<size_t>(entries_bytes));
        print_line(console, "mkpart: failed to write GPT entries");
        return false;
    }
    unmap(entries, static_cast<size_t>(entries_bytes));

    const uint8_t disk_guid[16] = {
        0x4E, 0x54, 0x52, 0x4E, 0x44, 0x49, 0x53, 0x4B,
        0x90, 0x01, 0x12, 0x34, 0x56, 0x78, 0x00, 0x01};
    write_gpt_header(sector,
                     1,
                     backup_header_lba,
                     first_usable_lba,
                     last_usable_lba,
                     primary_entries_lba,
                     entries_crc,
                     disk_guid);
    if (write_with_retry(handle,
                         sector,
                         static_cast<size_t>(geom.sector_size),
                         geom.sector_size) != static_cast<long>(geom.sector_size)) {
        print_line(console, "mkpart: failed to write primary GPT header");
        return false;
    }

    write_gpt_header(sector,
                     backup_header_lba,
                     1,
                     first_usable_lba,
                     last_usable_lba,
                     backup_entries_lba,
                     entries_crc,
                     disk_guid);
    if (write_with_retry(handle,
                         sector,
                         static_cast<size_t>(geom.sector_size),
                         backup_header_lba * geom.sector_size) !=
        static_cast<long>(geom.sector_size)) {
        print_line(console, "mkpart: failed to write backup GPT header");
        return false;
    }

    print(console, "mkpart: created GPT ");
    print(console, args.device);
    print(console, "_0 start=");
    print_u64(console, start_lba);
    print(console, " sectors=");
    print_u64(console, sector_count);
    print_line(console, "");
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
        print_line(console, "usage: mkpart [--gpt|--mbr] [--wipe] <disk> [--type N] [--start LBA] [--sectors N]");
        print_line(console, "examples:");
        print_line(console, "  mkpart --wipe --gpt USBMS_0");
        print_line(console, "  mkpart --wipe --mbr USBMS_0 --type 0x83");
        print_line(console, "then:");
        print_line(console, "  mkneufs USBMS_0_0");
        return 1;
    }

    long device = descriptor_open(
        kDescBlock,
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(args.device)),
        0,
        0);
    if (device < 0) {
        print(console, "mkpart: unable to open ");
        print_line(console, args.device);
        return 1;
    }

    if (descriptor_test_flag(static_cast<uint32_t>(device),
                             static_cast<uint64_t>(
                                 descriptor_defs::Flag::Writable)) != 1) {
        print_line(console, "mkpart: block device is not writable");
        descriptor_close(static_cast<uint32_t>(device));
        return 1;
    }

    descriptor_defs::BlockGeometry geom{};
    if (descriptor_get_property(
            static_cast<uint32_t>(device),
            static_cast<uint32_t>(descriptor_defs::Property::BlockGeometry),
            &geom,
            sizeof(geom)) != 0 ||
        geom.sector_size < 512 || geom.sector_size > 4096 ||
        geom.sector_count < 4096) {
        print_line(console, "mkpart: unsupported block geometry");
        descriptor_close(static_cast<uint32_t>(device));
        return 1;
    }

    uint8_t sector[4096];
    bool ok = prepare_overwrite(console,
                                static_cast<uint32_t>(device),
                                args,
                                geom,
                                sector);
    if (ok) {
        ok = args.table == TableKind::Gpt
                 ? write_gpt(console, static_cast<uint32_t>(device), args, geom, sector)
                 : write_mbr(console, static_cast<uint32_t>(device), args, geom, sector);
    }

    descriptor_close(static_cast<uint32_t>(device));
    if (!ok) {
        return 1;
    }
    if (rescan_block_devices() != 0) {
        print_line(console, "mkpart: created partition table but rescan failed");
        return 1;
    }
    return 0;
}
