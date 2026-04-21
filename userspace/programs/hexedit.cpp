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

constexpr size_t kDefaultCols = 80;
constexpr size_t kDefaultRows = 25;
constexpr size_t kMaxBytesPerRow = 16;
constexpr size_t kMinBytesPerRow = 4;
constexpr size_t kMaxPath = 128;
constexpr size_t kInputBuf = 64;
constexpr uint32_t kDefaultFg = 0xFFFFFFFF;
constexpr uint32_t kDefaultBg = 0x00000000;
constexpr uint32_t kHighlightFg = 0xFFFFA500;  // orange

bool set_cursor(long console, uint32_t x, uint32_t y) {
    descriptor_defs::CursorPosition pos{ x, y };
    long res = descriptor_set_property(
        static_cast<uint32_t>(console),
        static_cast<uint32_t>(descriptor_defs::Property::ConsoleCursor),
        &pos,
        sizeof(pos));
    return res == 0;
}

bool clear_console(long console) {
    long res = descriptor_set_property(
        static_cast<uint32_t>(console),
        static_cast<uint32_t>(descriptor_defs::Property::ConsoleClear),
        nullptr,
        0);
    return res == 0;
}

bool set_console_color(long console, uint32_t fg, uint32_t bg) {
    descriptor_defs::ColorPair colors{fg, bg};
    long res = descriptor_set_property(
        static_cast<uint32_t>(console),
        static_cast<uint32_t>(descriptor_defs::Property::ConsoleColor),
        &colors,
        sizeof(colors));
    return res == 0;
}

struct Buffer {
    uint8_t* data;
    size_t size;
    size_t capacity;
};

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

char hex_digit(uint8_t v) {
    return (v < 10) ? static_cast<char>('0' + v)
                    : static_cast<char>('A' + (v - 10));
}

void hex8(uint8_t value, char out[2]) {
    out[0] = hex_digit(static_cast<uint8_t>(value >> 4));
    out[1] = hex_digit(static_cast<uint8_t>(value & 0x0F));
}

void hex32(uint32_t value, char out[8]) {
    for (int i = 7; i >= 0; --i) {
        out[i] = hex_digit(static_cast<uint8_t>(value & 0x0F));
        value >>= 4;
    }
}

bool buffer_reserve(Buffer& buf, size_t new_cap) {
    if (new_cap <= buf.capacity) {
        return true;
    }
    uint8_t* fresh =
        static_cast<uint8_t*>(map_anonymous(new_cap, MAP_WRITE));
    if (fresh == nullptr) {
        return false;
    }
    memcpy(fresh, buf.data, buf.size);
    if (buf.data != nullptr && buf.capacity > 0) {
        unmap(buf.data, buf.capacity);
    }
    buf.data = fresh;
    buf.capacity = new_cap;
    return true;
}

bool buffer_append(Buffer& buf, const uint8_t* src, size_t count) {
    if (count == 0) {
        return true;
    }
    size_t needed = buf.size + count;
    size_t new_cap = buf.capacity;
    while (new_cap < needed) {
        new_cap = (new_cap == 0) ? 1024 : (new_cap * 2);
    }
    if (!buffer_reserve(buf, new_cap)) {
        return false;
    }
    for (size_t i = 0; i < count; ++i) {
        buf.data[buf.size + i] = src[i];
    }
    buf.size += count;
    return true;
}

bool load_file(const char* path, Buffer& out) {
    out = {};
    long h = file_open(path);
    if (h < 0) {
        return false;
    }
    uint8_t temp[512];
    while (1) {
        long r = file_read(static_cast<uint32_t>(h), temp, sizeof(temp));
        if (r < 0) {
            file_close(static_cast<uint32_t>(h));
            return false;
        }
        if (r == 0) {
            break;
        }
        if (!buffer_append(out, temp, static_cast<size_t>(r))) {
            file_close(static_cast<uint32_t>(h));
            return false;
        }
    }
    file_close(static_cast<uint32_t>(h));
    return true;
}

bool save_file(const char* path, const Buffer& buf) {
    long h = file_create(path);
    if (h < 0) {
        // Likely exists already; fall back to opening in place.
        h = file_open(path);
    }
    if (h < 0) {
        return false;
    }
    size_t written = 0;
    while (written < buf.size) {
        size_t chunk = buf.size - written;
        if (chunk > 512) {
            chunk = 512;
        }
        long w = file_write(static_cast<uint32_t>(h),
                            buf.data + written,
                            chunk);
        if (w < 0) {
            file_close(static_cast<uint32_t>(h));
            return false;
        }
        written += static_cast<size_t>(w);
    }
    file_close(static_cast<uint32_t>(h));
    return true;
}

bool parse_hex_byte(const char* text, uint8_t& out_val) {
    if (text == nullptr) return false;
    uint8_t accum = 0;
    size_t count = 0;
    while (*text != '\0' && count < 2) {
        char c = *text++;
        uint8_t v;
        if (c >= '0' && c <= '9') v = static_cast<uint8_t>(c - '0');
        else if (c >= 'a' && c <= 'f') v = static_cast<uint8_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v = static_cast<uint8_t>(c - 'A' + 10);
        else return false;
        accum = static_cast<uint8_t>((accum << 4) | v);
        ++count;
    }
    if (count == 0) return false;
    out_val = accum;
    return true;
}

uint64_t parse_number(const char* text) {
    if (text == nullptr || *text == '\0') {
        return 0;
    }
    bool hex = false;
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        hex = true;
        text += 2;
    } else {
        const char* scan = text;
        while (*scan != '\0') {
            char c = *scan++;
            if ((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) {
                hex = true;
                break;
            }
        }
    }
    uint64_t value = 0;
    while (*text != '\0') {
        char c = *text++;
        uint8_t v;
        if (c >= '0' && c <= '9') {
            v = static_cast<uint8_t>(c - '0');
        } else if (hex && c >= 'a' && c <= 'f') {
            v = static_cast<uint8_t>(c - 'a' + 10);
        } else if (hex && c >= 'A' && c <= 'F') {
            v = static_cast<uint8_t>(c - 'A' + 10);
        } else {
            break;
        }
        if (hex) {
            value = (value << 4) | v;
        } else {
            value = value * 10 + v;
        }
    }
    return value;
}

bool query_console_size(uint32_t& cols, uint32_t& rows) {
    long fb = framebuffer_open();
    if (fb < 0) {
        return false;
    }
    descriptor_defs::FramebufferInfo info{};
    bool ok = descriptor_get_property(
                  static_cast<uint32_t>(fb),
                  static_cast<uint32_t>(descriptor_defs::Property::FramebufferInfo),
                  &info,
                  sizeof(info)) == 0;
    descriptor_close(static_cast<uint32_t>(fb));
    if (!ok) {
        return false;
    }
    // Console uses 8x8 font scaled by 2 with extra spacing (approx 16x22 cells)
    const uint32_t cell_w = 16;
    const uint32_t cell_h = 22;
    if (info.width >= cell_w) {
        cols = info.width / cell_w;
    }
    if (info.height >= cell_h) {
        rows = info.height / cell_h;
    }
    return true;
}

size_t compute_bytes_per_row(uint32_t cols) {
    if (cols < 20) {
        cols = 20;
    }
    // rough worst‑case width: prefix (offset + separators) = 11 chars,
    // hex column worst case with cursor highlighting = 4 chars/byte ("[HH]" or "HH "),
    // ASCII column worst case with highlighting = 3 chars/byte ("[c]" or "c"),
    // plus final '|' = 1.
    // total ≈ 12 + 7*bytes_per_row
    size_t max_bytes = (cols > 12) ? static_cast<size_t>((cols - 12) / 7) : kMinBytesPerRow;
    if (max_bytes < kMinBytesPerRow) {
        max_bytes = kMinBytesPerRow;
    }
    if (max_bytes > kMaxBytesPerRow) {
        max_bytes = kMaxBytesPerRow;
    }
    return max_bytes;
}

size_t compute_view_rows(uint32_t rows) {
    if (rows <= 2) {
        return 1;
    }
    size_t view = static_cast<size_t>(rows - 2);  // header + help
    if (view > 40) {
        view = 40;
    }
    return view;
}

size_t clamp_cursor(const Buffer& buf, size_t cursor) {
    if (buf.size == 0) {
        return 0;
    }
    if (cursor >= buf.size) {
        return buf.size - 1;
    }
    return cursor;
}

void render_view(long console,
                 const Buffer& buf,
                 size_t cursor,
                 const char* path,
                 bool dirty,
                 size_t bytes_per_row,
                 size_t view_rows,
                 uint32_t cols) {
    clear_console(console);
    bool color_ok = set_console_color(console, kDefaultFg, kDefaultBg);
    char head[96];
    size_t idx = 0;
    const char* prefix = "hexedit ";
    while (prefix[idx] != '\0' && idx + 1 < sizeof(head)) {
        head[idx] = prefix[idx];
        ++idx;
    }
    size_t path_len = strlen(path);
    for (size_t i = 0; i < path_len && idx + 1 < sizeof(head); ++i) {
        head[idx++] = path[i];
    }
    const char* mid = " size=";
    for (size_t i = 0; mid[i] != '\0' && idx + 1 < sizeof(head); ++i) {
        head[idx++] = mid[i];
    }
    // size as decimal
    char digits[24];
    size_t dlen = 0;
    size_t temp = buf.size;
    if (temp == 0) {
        digits[dlen++] = '0';
    } else {
        char rev[24];
        size_t r = 0;
        while (temp != 0 && r < sizeof(rev)) {
            rev[r++] = static_cast<char>('0' + (temp % 10));
            temp /= 10;
        }
        while (r > 0 && dlen + 1 < sizeof(digits)) {
            digits[dlen++] = rev[--r];
        }
    }
    for (size_t i = 0; i < dlen && idx + 1 < sizeof(head); ++i) {
        head[idx++] = digits[i];
    }
    const char* tail = " bytes ";
    for (size_t i = 0; tail[i] != '\0' && idx + 1 < sizeof(head); ++i) {
        head[idx++] = tail[i];
    }
    if (dirty && idx + 1 < sizeof(head)) {
        head[idx++] = '*';
    }
    head[idx] = '\0';
    set_cursor(console, 0, 0);
    descriptor_write(static_cast<uint32_t>(console), head, strlen(head));
    size_t head_len = strlen(head);
    if (head_len < cols) {
        size_t pad = cols - head_len;
        for (size_t i = 0; i < pad; ++i) {
            descriptor_write(static_cast<uint32_t>(console), " ", 1);
        }
    }

    size_t start_row = (cursor / bytes_per_row);
    if (start_row > view_rows / 2) {
        start_row -= view_rows / 2;
    } else {
        start_row = 0;
    }

    for (size_t row = 0; row < view_rows; ++row) {
        size_t line_offset = (start_row + row) * bytes_per_row;
        if (line_offset >= (buf.size ? buf.size : 1)) {
            break;
        }
        char line[160];
        size_t p = 0;
        line[p++] = (cursor >= line_offset &&
                     cursor < line_offset + bytes_per_row) ? '>' : ' ';
        char off_hex[8];
        hex32(static_cast<uint32_t>(line_offset), off_hex);
        for (size_t i = 0; i < 8; ++i) line[p++] = off_hex[i];
        line[p++] = ':';
        line[p++] = ' ';

        for (size_t col = 0; col < bytes_per_row; ++col) {
            size_t idx_byte = line_offset + col;
            if (idx_byte < buf.size) {
                char h[2];
                hex8(buf.data[idx_byte], h);
                bool highlight = (idx_byte == cursor);
                if (color_ok) {
                    line[p++] = h[0];
                    line[p++] = h[1];
                    line[p++] = ' ';
                } else {
                    if (highlight) line[p++] = '[';
                    line[p++] = h[0];
                    line[p++] = h[1];
                    if (highlight) line[p++] = ']';
                    else line[p++] = ' ';
                }
            } else {
                line[p++] = ' ';
                line[p++] = ' ';
                line[p++] = ' ';
            }
        }
        line[p++] = ' ';
        line[p++] = '|';
        for (size_t col = 0; col < bytes_per_row; ++col) {
            size_t idx_byte = line_offset + col;
            char ch = ' ';
            if (idx_byte < buf.size) {
                uint8_t b = buf.data[idx_byte];
                if (b >= 0x20 && b <= 0x7E) ch = static_cast<char>(b);
                else ch = '.';
            }
            if (!color_ok && idx_byte == cursor) {
                line[p++] = '[';
                line[p++] = ch;
                line[p++] = ']';
            } else {
                line[p++] = ch;
            }
        }
        line[p++] = '|';
        line[p] = '\0';
        set_cursor(console, 0, 1 + row);
        size_t len = strlen(line);
        if (len > cols) {
            len = cols;
        }
        descriptor_write(static_cast<uint32_t>(console), line, len);
        if (len < cols) {
            size_t pad = cols - len;
            for (size_t i = 0; i < pad; ++i) {
                descriptor_write(static_cast<uint32_t>(console), " ", 1);
            }
        }

        if (color_ok &&
            cursor >= line_offset &&
            cursor < line_offset + bytes_per_row) {
            size_t col_offset = cursor - line_offset;
            uint32_t hex_x = static_cast<uint32_t>(1 + 8 + 1 + 1 + col_offset * 3);
            uint32_t ascii_x = static_cast<uint32_t>(1 + 8 + 1 + 1 + bytes_per_row * 3 + 2 + col_offset);
            set_console_color(console, kHighlightFg, kDefaultBg);
            set_cursor(console, hex_x, static_cast<uint32_t>(1 + row));
            char h[2];
            hex8(buf.data[cursor], h);
            descriptor_write(static_cast<uint32_t>(console), h, 2);
            set_cursor(console, ascii_x, static_cast<uint32_t>(1 + row));
            char ch = ' ';
            uint8_t b = buf.data[cursor];
            if (b >= 0x20 && b <= 0x7E) ch = static_cast<char>(b);
            else ch = '.';
            descriptor_write(static_cast<uint32_t>(console), &ch, 1);
            set_console_color(console, kDefaultFg, kDefaultBg);
        }
    }

    const char* help =
        "q quit | s save | g goto | e hex edit | a ascii | arrows/hjkl move";
    set_cursor(console, 0, static_cast<uint32_t>(1 + view_rows));
    size_t help_len = strlen(help);
    if (help_len > cols) help_len = cols;
    descriptor_write(static_cast<uint32_t>(console),
                     help,
                     help_len);
    if (help_len < cols) {
        size_t pad = cols - help_len;
        for (size_t i = 0; i < pad; ++i) {
            descriptor_write(static_cast<uint32_t>(console), " ", 1);
        }
    }
}

char read_char_blocking(uint32_t keyboard) {
    while (1) {
        descriptor_defs::KeyboardEvent events[8]{};
        long r = descriptor_read(keyboard, events, sizeof(events));
        if (r <= 0) {
            yield();
            continue;
        }
        size_t count = static_cast<size_t>(r) / sizeof(events[0]);
        for (size_t i = 0; i < count; ++i) {
            if (!keyboard::is_pressed(events[i])) {
                continue;
            }
            if (!keyboard::is_extended(events[i])) {
                char c = keyboard::scancode_to_char(events[i].scancode,
                                                    events[i].mods);
                if (c != 0) {
                    return c;
                }
            }
        }
    }
}

size_t read_line(uint32_t keyboard, long console, char* out, size_t out_cap) {
    if (out == nullptr || out_cap == 0) {
        return 0;
    }
    size_t len = 0;
    while (1) {
        char c = read_char_blocking(keyboard);
        if (c == '\n' || c == '\r') {
            descriptor_write(static_cast<uint32_t>(console), "\n", 1);
            break;
        } else if (c == '\b' || c == 0x7F) {
            if (len > 0) {
                --len;
                out[len] = '\0';
                descriptor_write(static_cast<uint32_t>(console), "\b \b", 3);
            }
        } else if (c >= 0x20 && c <= 0x7E) {
            if (len + 1 < out_cap) {
                out[len++] = c;
                out[len] = '\0';
                descriptor_write(static_cast<uint32_t>(console), &c, 1);
            }
        }
    }
    out[len] = '\0';
    return len;
}

bool prompt_yes_no(uint32_t keyboard, long console, const char* prompt) {
    print(console, prompt);
    print(console, " [y/N]: ");
    char c = read_char_blocking(keyboard);
    descriptor_write(static_cast<uint32_t>(console), "\n", 1);
    return c == 'y' || c == 'Y';
}

void prompt_string(uint32_t keyboard,
                   long console,
                   const char* prompt,
                   char* out,
                   size_t out_cap) {
    print(console, prompt);
    print(console, ": ");
    read_line(keyboard, console, out, out_cap);
}

}  // namespace

int main(uint64_t arg_ptr, uint64_t) {
    const char* path = reinterpret_cast<const char*>(arg_ptr);
    if (path == nullptr || path[0] == '\0') {
        return 1;
    }

    long console = descriptor_open(kDescConsole, 0);
    long keyboard = descriptor_open(kDescKeyboard, 0);
    if (console < 0 || keyboard < 0) {
        return 1;
    }

    Buffer buf{};
    if (!load_file(path, buf)) {
        print_line(console, "hexedit: failed to open file");
        return 1;
    }

    uint32_t cols = kDefaultCols;
    uint32_t rows = kDefaultRows;
    query_console_size(cols, rows);
    size_t bytes_per_row = compute_bytes_per_row(cols);
    size_t view_rows = compute_view_rows(rows);

    size_t cursor = 0;
    bool dirty = false;
    render_view(console,
                buf,
                cursor,
                path,
                dirty,
                bytes_per_row,
                view_rows,
                cols);

    while (1) {
        descriptor_defs::KeyboardEvent events[8]{};
        long r = descriptor_read(static_cast<uint32_t>(keyboard),
                                 events,
                                 sizeof(events));
        if (r <= 0) {
            yield();
            continue;
        }
        size_t count = static_cast<size_t>(r) / sizeof(events[0]);
        bool redraw = false;
        for (size_t i = 0; i < count; ++i) {
            const auto& ev = events[i];
            if (!keyboard::is_pressed(ev)) {
                continue;
            }
            int32_t dx = 0;
            int32_t dy = 0;
            if (keyboard::is_arrow_key(ev, dx, dy)) {
                if (dx < 0 && cursor > 0) cursor -= 1;
                if (dx > 0) cursor = clamp_cursor(buf, cursor + 1);
                if (dy < 0 && cursor >= bytes_per_row) cursor -= bytes_per_row;
                if (dy > 0) cursor = clamp_cursor(buf, cursor + bytes_per_row);
                redraw = true;
                continue;
            }
            char ch = keyboard::scancode_to_char(ev.scancode, ev.mods);
            if (ch == 0) {
                continue;
            }
            if (ch == 'q') {
                if (dirty) {
                    if (!prompt_yes_no(static_cast<uint32_t>(keyboard),
                                       console,
                                       "Unsaved changes, quit")) {
                        redraw = true;
                        continue;
                    }
                }
                if (buf.data && buf.capacity > 0) {
                    unmap(buf.data, buf.capacity);
                }
                return 0;
            } else if (ch == 's') {
                if (save_file(path, buf)) {
                    dirty = false;
                    print_line(console, "saved");
                } else {
                    print_line(console, "save failed");
                }
                redraw = true;
            } else if (ch == 'g') {
                char input[kInputBuf];
                prompt_string(static_cast<uint32_t>(keyboard),
                              console,
                              "goto offset (hex or dec)",
                              input,
                              sizeof(input));
                uint64_t target = parse_number(input);
                cursor = clamp_cursor(buf, static_cast<size_t>(target));
                redraw = true;
            } else if (ch == 'e') {
                char input[4];
                prompt_string(static_cast<uint32_t>(keyboard),
                              console,
                              "set byte (hex)",
                              input,
                              sizeof(input));
                uint8_t value = 0;
                if (parse_hex_byte(input, value)) {
                    if (cursor >= buf.size) {
                        buffer_append(buf, &value, 1);
                    } else {
                        buf.data[cursor] = value;
                    }
                    dirty = true;
                } else {
                    print_line(console, "invalid hex");
                }
                redraw = true;
            } else if (ch == 'a') {
                print(console, "set ascii: ");
                char c = read_char_blocking(static_cast<uint32_t>(keyboard));
                descriptor_write(static_cast<uint32_t>(console), "\n", 1);
                uint8_t v = static_cast<uint8_t>(c);
                if (cursor >= buf.size) {
                    buffer_append(buf, &v, 1);
                } else {
                    buf.data[cursor] = v;
                }
                dirty = true;
                redraw = true;
            } else if (ch == 'h') {
                if (cursor > 0) cursor -= 1;
                redraw = true;
            } else if (ch == 'l') {
                cursor = clamp_cursor(buf, cursor + 1);
                redraw = true;
            } else if (ch == 'k') {
                if (cursor >= bytes_per_row) cursor -= bytes_per_row;
                redraw = true;
            } else if (ch == 'j') {
                cursor = clamp_cursor(buf, cursor + bytes_per_row);
                redraw = true;
            }
        }
        if (redraw) {
            render_view(console,
                        buf,
                        cursor,
                        path,
                        dirty,
                        bytes_per_row,
                        view_rows,
                        cols);
        }
    }
}
