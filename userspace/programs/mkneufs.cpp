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
constexpr long kWouldBlock = -2;

constexpr uint8_t kNeufsMagic[8] = {
    0x4E, 0x45, 0x55, 0x46, 0x53, 0x00, 0x77, 0x42};
constexpr int32_t kNeufsVersion = 1;
constexpr uint8_t kTypeNdir = 0;

#pragma pack(push, 1)
struct NeufsRvt {
    char magic[8];
    int32_t version;
    char name[16];
    uint64_t root;
};

struct NeufsNdir {
    uint8_t type;
    uint8_t reserved[7];
    char name[256];
    int64_t ctime;
    int64_t utime;
    uint64_t acl[32];
    uint64_t parent;
    uint64_t contents[64];
    uint64_t next;
    uint64_t last;
};
#pragma pack(pop)

static_assert(sizeof(NeufsRvt) % 4 == 0);
static_assert(sizeof(NeufsNdir) % 4 == 0);

struct Args {
    char device[64];
    char label[16];
};

void print_progress(long console, const char* action, uint64_t done, uint64_t total) {
    uint64_t percent = total == 0 ? 100 : (done * 100) / total;
    constexpr uint64_t mib = 1024ull * 1024ull;
    userspace::write(console, "mkneufs: ");
    userspace::write(console, action);
    userspace::write(console, " ");
    userspace::write_u64(console, percent);
    userspace::write(console, "% (");
    userspace::write_u64(console, done / mib);
    userspace::write(console, "/");
    userspace::write_u64(console, (total + mib - 1) / mib);
    userspace::write_line(console, " MiB)");
}

bool parse_args(const char* raw, Args& out) {
    memset(&out, 0, sizeof(out));
    if (raw == nullptr || raw[0] == '\0') {
        return false;
    }

    const char* cursor = raw;
    if (!userspace::copy_token(cursor, out.device, sizeof(out.device))) {
        return false;
    }

    const char* after_device = userspace::skip_spaces(cursor);
    if (after_device != nullptr && *after_device != '\0') {
        cursor = after_device;
        if (!userspace::copy_token(cursor, out.label, sizeof(out.label))) {
            return false;
        }
        cursor = userspace::skip_spaces(cursor);
        return cursor == nullptr || *cursor == '\0';
    }

    const char* default_label = "neufs";
    memcpy(out.label, default_label, strlen(default_label) + 1);
    return true;
}

uint64_t align_up(uint64_t value, uint64_t alignment) {
    if (alignment == 0) {
        return value;
    }
    uint64_t rem = value % alignment;
    return rem == 0 ? value : value + (alignment - rem);
}

uint64_t default_meta_size(uint64_t total_bytes, uint64_t sector_size) {
    uint64_t suggested = (total_bytes * 225) / 10000;
    const uint64_t min_meta = 256ull * 1024ull * 1024ull;
    const uint64_t max_meta = 16ull * 1024ull * 1024ull * 1024ull;
    if (suggested < min_meta && total_bytes >= min_meta * 2) {
        suggested = min_meta;
    }
    if (suggested > max_meta) {
        suggested = max_meta;
    }
    if (sector_size != 0 && suggested > total_bytes - sector_size) {
        suggested = total_bytes - sector_size;
    } else if (sector_size == 0 && suggested > total_bytes) {
        suggested = total_bytes;
    }
    return align_up(suggested, sector_size);
}

long read_exact(uint32_t handle, void* buffer, size_t length, uint64_t offset) {
    while (true) {
        long result = descriptor_read(handle, buffer, length, offset);
        if (result == kWouldBlock) {
            yield();
            continue;
        }
        return result;
    }
}

long write_exact(uint32_t handle,
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

bool zero_region(long console,
                 uint32_t handle,
                 uint64_t offset,
                 uint64_t size,
                 uint8_t* scratch,
                 uint64_t scratch_size) {
    uint64_t total_size = size;
    uint64_t done = 0;
    constexpr uint64_t progress_step = 1024ull * 1024ull;
    uint64_t next_progress = progress_step;

    memset(scratch, 0, static_cast<size_t>(scratch_size));
    print_progress(console, "clearing metadata", 0, total_size);
    while (size > 0) {
        uint64_t chunk = size < scratch_size ? size : scratch_size;
        if (write_exact(handle,
                        scratch,
                        static_cast<size_t>(chunk),
                        offset) != static_cast<long>(chunk)) {
            return false;
        }
        offset += chunk;
        size -= chunk;
        done += chunk;

        if (done >= next_progress || size == 0) {
            print_progress(console, "clearing metadata", done, total_size);
            while (next_progress <= done) {
                next_progress += progress_step;
            }
        }
    }
    return true;
}

bool write_bytes(uint32_t handle,
                 uint64_t offset,
                 const void* data,
                 size_t size,
                 uint8_t* sector,
                 uint64_t sector_size) {
    const uint8_t* src = static_cast<const uint8_t*>(data);
    while (size > 0) {
        uint64_t sector_offset = (offset / sector_size) * sector_size;
        size_t in_sector = static_cast<size_t>(offset - sector_offset);
        size_t chunk = static_cast<size_t>(sector_size) - in_sector;
        if (chunk > size) {
            chunk = size;
        }

        if (read_exact(handle,
                       sector,
                       static_cast<size_t>(sector_size),
                       sector_offset) != static_cast<long>(sector_size)) {
            return false;
        }
        memcpy(sector + in_sector, src, chunk);
        if (write_exact(handle,
                        sector,
                        static_cast<size_t>(sector_size),
                        sector_offset) != static_cast<long>(sector_size)) {
            return false;
        }

        offset += chunk;
        src += chunk;
        size -= chunk;
    }
    return true;
}

bool bitmap_set_range(uint8_t* sector,
                      uint32_t handle,
                      uint64_t bitmap_offset,
                      uint64_t start_bit,
                      uint64_t count,
                      uint64_t sector_size) {
    uint64_t end_bit = start_bit + count;
    uint64_t current_bit = start_bit;

    while (current_bit < end_bit) {
        uint64_t byte_offset = bitmap_offset + (current_bit / 8);
        uint64_t sector_offset = (byte_offset / sector_size) * sector_size;
        if (read_exact(handle,
                       sector,
                       static_cast<size_t>(sector_size),
                       sector_offset) != static_cast<long>(sector_size)) {
            return false;
        }

        while (current_bit < end_bit) {
            uint64_t current_byte = bitmap_offset + (current_bit / 8);
            if ((current_byte / sector_size) * sector_size != sector_offset) {
                break;
            }
            size_t in_sector = static_cast<size_t>(current_byte - sector_offset);
            uint8_t bit_in_byte = static_cast<uint8_t>(current_bit % 8);
            uint8_t mask = 0;
            while (bit_in_byte < 8 && current_bit < end_bit) {
                mask |= static_cast<uint8_t>(1u << bit_in_byte);
                ++bit_in_byte;
                ++current_bit;
            }
            sector[in_sector] |= mask;
        }

        if (write_exact(handle,
                        sector,
                        static_cast<size_t>(sector_size),
                        sector_offset) != static_cast<long>(sector_size)) {
            return false;
        }
    }
    return true;
}

bool format_neufs(long console,
                  uint32_t handle,
                  const Args& args,
                  const descriptor_defs::BlockGeometry& geom) {
    if (geom.sector_size == 0 || geom.sector_count == 0 ||
        geom.sector_size > 4096) {
        userspace::write_line(console, "mkneufs: unsupported block geometry");
        return false;
    }

    uint64_t total_bytes = geom.sector_size * geom.sector_count;
    uint64_t meta_size = default_meta_size(total_bytes, geom.sector_size);
    if (meta_size >= total_bytes) {
        userspace::write_line(console, "mkneufs: device too small for spec default metadata area");
        return false;
    }

    uint64_t data_bitmap_offset = align_up(sizeof(NeufsRvt), 8);
    uint64_t data_bitmap_size = (geom.sector_count + 7) / 8;
    uint64_t meta_blocks = meta_size / 8;
    uint64_t meta_bitmap_offset =
        align_up(data_bitmap_offset + data_bitmap_size, 8);
    uint64_t meta_bitmap_size = (meta_blocks + 7) / 8;
    uint64_t bitmap_end = meta_bitmap_offset + meta_bitmap_size;
    uint64_t root_offset = align_up(bitmap_end, geom.sector_size);
    if (root_offset + sizeof(NeufsNdir) > meta_size) {
        userspace::write_line(console, "mkneufs: metadata area cannot fit root directory");
        return false;
    }

    uint64_t sectors_per_write = 255;
    uint64_t scratch_size = geom.sector_size * sectors_per_write;
    uint8_t* sector = static_cast<uint8_t*>(
        map_anonymous(static_cast<size_t>(scratch_size), MAP_WRITE));
    if (sector == nullptr) {
        userspace::write_line(console, "mkneufs: allocation failed");
        return false;
    }

    if (!zero_region(console, handle, 0, meta_size, sector, scratch_size)) {
        userspace::write_line(console, "mkneufs: failed to clear metadata area");
        unmap(sector, static_cast<size_t>(scratch_size));
        return false;
    }

    userspace::write_line(console, "mkneufs: writing RVT");
    NeufsRvt rvt{};
    for (size_t i = 0; i < sizeof(kNeufsMagic); ++i) {
        rvt.magic[i] = static_cast<char>(kNeufsMagic[i]);
    }
    rvt.version = kNeufsVersion;
    memcpy(rvt.name, args.label, strlen(args.label));
    rvt.root = root_offset;
    if (!write_bytes(handle, 0, &rvt, sizeof(rvt), sector, geom.sector_size)) {
        userspace::write_line(console, "mkneufs: failed to write RVT");
        unmap(sector, static_cast<size_t>(scratch_size));
        return false;
    }

    userspace::write_line(console, "mkneufs: writing root directory");
    NeufsNdir root{};
    root.type = kTypeNdir;
    root.name[0] = '/';
    root.name[1] = '\0';
    if (!write_bytes(handle,
                     root_offset,
                     &root,
                     sizeof(root),
                     sector,
                     geom.sector_size)) {
        userspace::write_line(console, "mkneufs: failed to write root directory");
        unmap(sector, static_cast<size_t>(scratch_size));
        return false;
    }

    uint64_t used_data_sectors = align_up(meta_size, geom.sector_size) /
                                 geom.sector_size;
    userspace::write_line(console, "mkneufs: initializing data bitmap");
    if (!bitmap_set_range(sector,
                          handle,
                          data_bitmap_offset,
                          0,
                          used_data_sectors,
                          geom.sector_size)) {
        userspace::write_line(console, "mkneufs: failed to initialize data bitmap");
        unmap(sector, static_cast<size_t>(scratch_size));
        return false;
    }

    uint64_t rvt_blocks = align_up(sizeof(NeufsRvt), 8) / 8;
    uint64_t data_bitmap_blocks = align_up(data_bitmap_size, 8) / 8;
    uint64_t meta_bitmap_blocks = align_up(meta_bitmap_size, 8) / 8;
    uint64_t root_blocks = align_up(sizeof(NeufsNdir), 8) / 8;
    userspace::write_line(console, "mkneufs: initializing metadata bitmap");
    if (!bitmap_set_range(sector, handle, meta_bitmap_offset, 0,
                          rvt_blocks, geom.sector_size) ||
        !bitmap_set_range(sector, handle, meta_bitmap_offset,
                          data_bitmap_offset / 8,
                          data_bitmap_blocks,
                          geom.sector_size) ||
        !bitmap_set_range(sector, handle, meta_bitmap_offset,
                          meta_bitmap_offset / 8,
                          meta_bitmap_blocks,
                          geom.sector_size) ||
        !bitmap_set_range(sector, handle, meta_bitmap_offset,
                          root_offset / 8,
                          root_blocks,
                          geom.sector_size)) {
        userspace::write_line(console, "mkneufs: failed to initialize metadata bitmap");
        unmap(sector, static_cast<size_t>(scratch_size));
        return false;
    }

    unmap(sector, static_cast<size_t>(scratch_size));

    userspace::write(console, "mkneufs: formatted ");
    userspace::write(console, args.device);
    userspace::write(console, " label=");
    userspace::write_line(console, args.label);
    userspace::write(console, "mkneufs: sectors=");
    userspace::write_u64(console, geom.sector_count);
    userspace::write(console, " sector_size=");
    userspace::write_u64(console, geom.sector_size);
    userspace::write(console, " meta_bytes=");
    userspace::write_u64(console, meta_size);
    userspace::write(console, " root=");
    userspace::write_u64(console, root_offset);
    userspace::write_line(console, "");
    return true;
}

}  // namespace

int main(uint64_t arg_ptr, uint64_t) {
    long console = process_get_standard_descriptor(1);
    if (console < 0) {
        console = descriptor_open(kDescConsole, 0);
    }

    Args args{};
    const char* raw = reinterpret_cast<const char*>(arg_ptr);
    if (!parse_args(raw, args)) {
        userspace::write_line(console, "usage: mkneufs <block-device> [label]");
        userspace::write_line(console, "example: mkneufs IDE_SM_0 scratch");
        return 1;
    }

    long device = descriptor_open(
        kDescBlock,
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(args.device)),
        0,
        0);

    if (device < 0) {
        userspace::write(console, "mkneufs: unable to open ");
        userspace::write_line(console, args.device);
        return 1;
    }

    if (descriptor_test_flag(static_cast<uint32_t>(device),
                             static_cast<uint64_t>(
                                 descriptor_defs::Flag::Writable)) != 1) {
        userspace::write_line(console, "mkneufs: block device is not writable");
        descriptor_close(static_cast<uint32_t>(device));
        return 1;
    }

    descriptor_defs::BlockGeometry geom{};
    if (descriptor_get_property(
            static_cast<uint32_t>(device),
            static_cast<uint32_t>(descriptor_defs::Property::BlockGeometry),
            &geom,
            sizeof(geom)) != 0) {
        userspace::write_line(console, "mkneufs: failed to query block geometry");
        descriptor_close(static_cast<uint32_t>(device));
        return 1;
    }

    bool ok = format_neufs(console,
                           static_cast<uint32_t>(device),
                           args,
                           geom);
    descriptor_close(static_cast<uint32_t>(device));
    return ok ? 0 : 1;
}
