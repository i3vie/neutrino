#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "descriptors.hpp"
#include "keyboard_scancode.hpp"
#include "../crt/syscall.hpp"

namespace {

constexpr uint32_t kDescConsole =
    static_cast<uint32_t>(descriptor_defs::Type::Console);
constexpr uint32_t kDescKeyboard =
    static_cast<uint32_t>(descriptor_defs::Type::Keyboard);
constexpr uint32_t kDescBlock =
    static_cast<uint32_t>(descriptor_defs::Type::BlockDevice);

constexpr uint32_t kDefaultFg = 0xFFFFFFFF;
constexpr uint32_t kDefaultBg = 0x00000000;
constexpr long kWouldBlock = -2;

struct Device {
    char name[64];
    descriptor_defs::BlockGeometry geom;
    bool writable;
    bool is_memdisk;
};

bool set_cursor(long console, uint32_t x, uint32_t y) {
    descriptor_defs::CursorPosition pos{x, y};
    long res = descriptor_set_property(
        static_cast<uint32_t>(console),
        static_cast<uint32_t>(descriptor_defs::Property::ConsoleCursor),
        &pos, sizeof(pos));
    return res == 0;
}

bool clear_console(long console) {
    long res = descriptor_set_property(
        static_cast<uint32_t>(console),
        static_cast<uint32_t>(descriptor_defs::Property::ConsoleClear),
        nullptr, 0);
    return res == 0;
}

bool set_console_color(long console, uint32_t fg, uint32_t bg) {
    descriptor_defs::ColorPair colors{fg, bg};
    long res = descriptor_set_property(
        static_cast<uint32_t>(console),
        static_cast<uint32_t>(descriptor_defs::Property::ConsoleColor),
        &colors, sizeof(colors));
    return res == 0;
}

void print(long console, const char* text) {
    if (console < 0 || text == nullptr) {
        return;
    }
    descriptor_write(static_cast<uint32_t>(console),
                     text,
                     strlen(text));
}

void print_line(long console, const char* text) {
    print(console, text);
    descriptor_write(static_cast<uint32_t>(console), "\n", 1);
}

char read_char_blocking(uint32_t keyboard) {
    descriptor_defs::KeyboardEvent events[8]{};
    while (true) {
        long r = descriptor_read(keyboard, events, sizeof(events));
        if (r <= 0) {
            yield();
            continue;
        }
        size_t count = static_cast<size_t>(r) / sizeof(events[0]);
        for (size_t i = 0; i < count; ++i) {
            const auto& ev = events[i];
            if (!keyboard::is_pressed(ev)) {
                continue;
            }
            char c = keyboard::scancode_to_char(ev.scancode, ev.mods);
            if (c != 0) {
                return c;
            }
        }
    }
}

void drain_keyboard(uint32_t keyboard) {
    descriptor_defs::KeyboardEvent events[8];
    while (descriptor_read(keyboard, events, sizeof(events)) > 0) {
        // discard
    }
}

size_t read_line(uint32_t keyboard,
                 long console,
                 char* out,
                 size_t out_cap) {
    if (out == nullptr || out_cap == 0) {
        return 0;
    }
    size_t len = 0;
    while (len + 1 < out_cap) {
        char c = read_char_blocking(keyboard);
        if (c == '\n' || c == '\r') {
            descriptor_write(static_cast<uint32_t>(console), "\n", 1);
            break;
        }
        if (c == '\b') {
            if (len > 0) {
                --len;
                descriptor_write(static_cast<uint32_t>(console), "\b \b", 3);
            }
            continue;
        }
        out[len++] = c;
        descriptor_write(static_cast<uint32_t>(console), &c, 1);
    }
    out[len] = '\0';
    return len;
}

bool prompt_yes_no_line(uint32_t keyboard, long console, const char* prompt) {
    char buf[8];
    print(console, prompt);
    print(console, " [y/N]: ");
    drain_keyboard(keyboard);
    size_t len = read_line(keyboard, console, buf, sizeof(buf));
    if (len == 0) {
        return false;
    }
    char c = buf[0];
    return c == 'y' || c == 'Y';
}

bool parse_uint(const char* text, size_t& out_value) {
    out_value = 0;
    if (text == nullptr || *text == '\0') {
        return false;
    }
    const char* cursor = text;
    while (*cursor != '\0') {
        char c = *cursor++;
        if (c < '0' || c > '9') {
            return false;
        }
        out_value = out_value * 10 + static_cast<size_t>(c - '0');
    }
    return true;
}

void format_size(uint64_t bytes, char* out, size_t out_cap) {
    if (out == nullptr || out_cap == 0) {
        return;
    }
    const uint64_t kGiB = 1024ull * 1024ull * 1024ull;
    const uint64_t kMiB = 1024ull * 1024ull;
    if (bytes >= kGiB) {
        uint64_t whole = bytes / kGiB;
        uint64_t frac = (bytes % kGiB) * 10 / kGiB;
        // format "<whole>.<frac> GiB"
        char buf[32];
        size_t idx = 0;
        // whole part
        char digits[20];
        size_t digit_count = 0;
        uint64_t temp = whole;
        do {
            digits[digit_count++] = static_cast<char>('0' + (temp % 10));
            temp /= 10;
        } while (temp > 0 && digit_count < sizeof(digits));
        for (size_t i = 0; i < digit_count && idx + 1 < sizeof(buf); ++i) {
            buf[idx++] = digits[digit_count - 1 - i];
        }
        if (idx + 3 < sizeof(buf)) {
            buf[idx++] = '.';
            buf[idx++] = static_cast<char>('0' + (frac % 10));
            buf[idx++] = ' ';
            buf[idx++] = 'G';
            buf[idx++] = 'i';
            buf[idx++] = 'B';
            buf[idx] = '\0';
        } else {
            buf[0] = '\0';
        }
        // copy to out
        size_t copy_len = strlen(buf);
        if (copy_len >= out_cap) {
            copy_len = out_cap - 1;
        }
        for (size_t i = 0; i < copy_len; ++i) {
            out[i] = buf[i];
        }
        out[copy_len] = '\0';
        return;
    }
    uint64_t whole = bytes / kMiB;
    char buf[32];
    size_t idx = 0;
    char digits[20];
    size_t digit_count = 0;
    uint64_t temp = whole;
    do {
        digits[digit_count++] = static_cast<char>('0' + (temp % 10));
        temp /= 10;
    } while (temp > 0 && digit_count < sizeof(digits));
    for (size_t i = 0; i < digit_count && idx + 1 < sizeof(buf); ++i) {
        buf[idx++] = digits[digit_count - 1 - i];
    }
    if (idx + 3 < sizeof(buf)) {
        buf[idx++] = ' ';
        buf[idx++] = 'M';
        buf[idx++] = 'i';
        buf[idx++] = 'B';
        buf[idx] = '\0';
    } else {
        buf[0] = '\0';
    }
    size_t copy_len = strlen(buf);
    if (copy_len >= out_cap) {
        copy_len = out_cap - 1;
    }
    for (size_t i = 0; i < copy_len; ++i) {
        out[i] = buf[i];
    }
    out[copy_len] = '\0';
}

bool fetch_devices(Device* devices, size_t capacity, size_t& count) {
    if (devices == nullptr || capacity == 0) {
        return false;
    }
    count = 0;
    for (size_t idx = 0; idx < capacity; ++idx) {
        long handle = descriptor_open(kDescBlock, 0, idx, 0);
        if (handle < 0) {
            break;
        }

        Device& dev = devices[count];
        for (size_t i = 0; i < sizeof(dev.name); ++i) {
            dev.name[i] = '\0';
        }
        descriptor_get_property(
            static_cast<uint32_t>(handle),
            static_cast<uint32_t>(descriptor_defs::Property::CommonName),
            dev.name, sizeof(dev.name));

        dev.geom = {};
        descriptor_get_property(
            static_cast<uint32_t>(handle),
            static_cast<uint32_t>(descriptor_defs::Property::BlockGeometry),
            &dev.geom, sizeof(dev.geom));

        dev.writable = descriptor_test_flag(
            static_cast<uint32_t>(handle),
            static_cast<uint64_t>(descriptor_defs::Flag::Writable)) == 1;

        dev.is_memdisk = strncmp(dev.name, "MEMDISK_", strlen("MEMDISK_")) == 0;

        descriptor_close(static_cast<uint32_t>(handle));
        ++count;
    }
    return count > 0;
}

long read_with_retry(uint32_t handle,
                     void* buffer,
                     size_t length,
                     uint64_t offset) {
    while (true) {
        long r = descriptor_read(handle, buffer, length, offset);
        if (r == kWouldBlock) {
            yield();
            continue;
        }
        return r;
    }
}

long write_with_retry(uint32_t handle,
                      const void* buffer,
                      size_t length,
                      uint64_t offset) {
    while (true) {
        long r = descriptor_write(handle, buffer, length, offset);
        if (r == kWouldBlock) {
            yield();
            continue;
        }
        return r;
    }
}

void render_table(long console,
                  const Device* devices,
                  size_t count,
                  size_t highlight_row,
                  size_t first_data_row) {
    set_cursor(console, 0, static_cast<uint32_t>(first_data_row));
    for (size_t i = 0; i < count; ++i) {
        char size_buf[32];
        uint64_t bytes =
            devices[i].geom.sector_size * devices[i].geom.sector_count;
        format_size(bytes, size_buf, sizeof(size_buf));

        if (i == highlight_row) {
            set_console_color(console, 0xFF000000, 0xFFFFFFFF);
        } else {
            set_console_color(console, kDefaultFg, kDefaultBg);
        }

        print(console, " ");
        char idx_buf[6];
        idx_buf[0] = '#';
        idx_buf[1] = '\0';
        size_t idx_len = 1;
        size_t idx = i;
        char digits[10];
        size_t digit_count = 0;
        do {
            digits[digit_count++] = static_cast<char>('0' + (idx % 10));
            idx /= 10;
        } while (idx > 0 && digit_count < sizeof(digits));
        for (size_t d = 0; d < digit_count && idx_len + 1 < sizeof(idx_buf);
             ++d) {
            idx_buf[idx_len++] = digits[digit_count - 1 - d];
        }
        idx_buf[idx_len] = '\0';
        print(console, idx_buf);
        print(console, "  ");
        print(console, devices[i].name);
        print(console, "  ");
        print(console, size_buf);
        print(console, "  ");
        print(console, devices[i].writable ? "[rw]" : "[ro]");
        if (devices[i].is_memdisk) {
            print(console, "  (source)");
        }
        descriptor_write(static_cast<uint32_t>(console), "\n", 1);
    }
    set_console_color(console, kDefaultFg, kDefaultBg);
}

bool copy_image(const char* src_name,
                const Device& src,
                const char* dst_name,
                const Device& dst,
                long console) {
    if (src_name == nullptr || dst_name == nullptr) {
        return false;
    }

    if (dst.geom.sector_size == 0 || src.geom.sector_size == 0 ||
        src.geom.sector_size != dst.geom.sector_size) {
        print_line(console, "sector size mismatch");
        return false;
    }
    if (dst.geom.sector_count < src.geom.sector_count) {
        print_line(console, "destination too small for image");
        return false;
    }
    uint64_t sector_size = src.geom.sector_size;
    uint64_t total_sectors = src.geom.sector_count;

    long src_handle = descriptor_open(
        kDescBlock,
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(src_name)),
        0, 0);
    long dst_handle = descriptor_open(
        kDescBlock,
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(dst_name)),
        0, 0);
    if (src_handle < 0 || dst_handle < 0) {
        print_line(console, "failed to open block devices");
        if (src_handle >= 0) descriptor_close(static_cast<uint32_t>(src_handle));
        if (dst_handle >= 0) descriptor_close(static_cast<uint32_t>(dst_handle));
        return false;
    }

    const uint64_t kChunkSectors = 1024;  // ~512KiB per transfer
    uint64_t chunk_bytes = kChunkSectors * sector_size;
    uint8_t* buffer = static_cast<uint8_t*>(
        map_anonymous(static_cast<size_t>(chunk_bytes), MAP_WRITE));
    if (buffer == nullptr) {
        print_line(console, "failed to allocate buffer");
        descriptor_close(static_cast<uint32_t>(src_handle));
        descriptor_close(static_cast<uint32_t>(dst_handle));
        return false;
    }

    uint64_t copied = 0;
    uint64_t last_pct = UINT64_MAX;
    print_line(console, "Starting install...");
    while (copied < total_sectors) {
        uint64_t batch = kChunkSectors;
        if (batch > total_sectors - copied) {
            batch = total_sectors - copied;
        }
        uint64_t bytes = batch * sector_size;
        uint64_t offset = copied * sector_size;

        long r = read_with_retry(static_cast<uint32_t>(src_handle),
                                 buffer,
                                 static_cast<size_t>(bytes),
                                 offset);
        if (r < 0 || static_cast<uint64_t>(r) != bytes) {
            print_line(console, "read failed");
            unmap(buffer, static_cast<size_t>(chunk_bytes));
            descriptor_close(static_cast<uint32_t>(src_handle));
            descriptor_close(static_cast<uint32_t>(dst_handle));
            return false;
        }
        long w = write_with_retry(static_cast<uint32_t>(dst_handle),
                                  buffer,
                                  static_cast<size_t>(bytes),
                                  offset);
        if (w < 0 || static_cast<uint64_t>(w) != bytes) {
            print_line(console, "write failed");
            unmap(buffer, static_cast<size_t>(chunk_bytes));
            descriptor_close(static_cast<uint32_t>(src_handle));
            descriptor_close(static_cast<uint32_t>(dst_handle));
            return false;
        }
        copied += batch;

        uint64_t percent = (copied * 100) / total_sectors;
        if (percent != last_pct) {
            last_pct = percent;
            set_cursor(console, 0, 6);
            print(console, "Installing... ");
            char pct[8];
            pct[0] = static_cast<char>('0' + (percent / 10));
            pct[1] = static_cast<char>('0' + (percent % 10));
            pct[2] = '%';
            pct[3] = '\0';
            print(console, pct);
            descriptor_write(static_cast<uint32_t>(console), "   \r", 4);
            yield();
        }
    }

    unmap(buffer, static_cast<size_t>(chunk_bytes));
    descriptor_close(static_cast<uint32_t>(src_handle));
    descriptor_close(static_cast<uint32_t>(dst_handle));
    set_cursor(console, 0, 6);
    print_line(console, "Install complete.              ");
    return true;
}

}  // namespace

int main(uint64_t, uint64_t) {
    long console = descriptor_open(kDescConsole, 0);
    long keyboard = descriptor_open(kDescKeyboard, 0);
    if (console < 0 || keyboard < 0) {
        return 1;
    }

    clear_console(console);
    set_console_color(console, kDefaultFg, kDefaultBg);
    print_line(console, "Neutrino Installer");
    print_line(console, "Select a target drive to format as FAT32 and install Neutrino.");

    Device devices[16];
    size_t device_count = 0;
    if (!fetch_devices(devices, 16, device_count) || device_count == 0) {
        print_line(console, "No block devices detected.");
        return 1;
    }

    size_t source_index = device_count;
    for (size_t i = 0; i < device_count; ++i) {
        if (devices[i].is_memdisk) {
            source_index = i;
            break;
        }
    }
    if (source_index == device_count) {
        print_line(console, "No MEMDISK image found; boot the live ISO.");
        return 1;
    }

    while (true) {
        set_cursor(console, 0, 3);
        print_line(console, "Available drives:");
        render_table(console, devices, device_count, source_index, 4);

        print_line(console, "");
        print(console, "Enter target index (or q to quit): ");
        char input[8];
        size_t len = read_line(static_cast<uint32_t>(keyboard),
                               console,
                               input,
                               sizeof(input));
        if (len == 1 && (input[0] == 'q' || input[0] == 'Q')) {
            return 0;
        }
        size_t choice = 0;
        if (!parse_uint(input, choice) || choice >= device_count ||
            devices[choice].is_memdisk) {
            print_line(console, "Invalid selection.");
            continue;
        }
        if (!devices[choice].writable) {
            print_line(console, "Drive is read-only.");
            continue;
        }

        bool proceed = prompt_yes_no_line(static_cast<uint32_t>(keyboard),
                                          console,
                                          "This will erase the drive. Proceed");
        if (!proceed) {
            print_line(console, "Cancelled.");
            continue;
        }

        bool ok = copy_image(devices[source_index].name,
                             devices[source_index],
                             devices[choice].name,
                             devices[choice],
                             console);
        if (!ok) {
            print_line(console, "Install failed.");
        }

        bool again = prompt_yes_no_line(static_cast<uint32_t>(keyboard),
                                        console,
                                        "Install another drive");
        if (!again) {
            break;
        }
    }

    return 0;
}
