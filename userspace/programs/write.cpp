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
constexpr uint32_t kDefaultFg = 0xFFF2F4F8u;
constexpr uint32_t kDefaultBg = 0x00000000u;
constexpr uint32_t kBarFg = 0xFF101820u;
constexpr uint32_t kBarBg = 0xFFB7D7FFu;
constexpr uint32_t kAccentFg = 0xFFFFD27Du;
constexpr size_t kMaxLines = 256;
constexpr size_t kLineCap = 192;
constexpr size_t kMaxPath = 128;

char g_lines[kMaxLines][kLineCap];
size_t g_line_count = 1;
size_t g_cursor_line = 0;
size_t g_cursor_col = 0;
size_t g_top_visual = 0;
bool g_dirty = false;
bool g_quit_armed = false;
char g_status[96] = "Ready";

bool set_cursor(long console, uint32_t x, uint32_t y) {
    descriptor_defs::CursorPosition pos{x, y};
    return descriptor_set_property(
               static_cast<uint32_t>(console),
               static_cast<uint32_t>(descriptor_defs::Property::ConsoleCursor),
               &pos,
               sizeof(pos)) == 0;
}

bool set_color(long console, uint32_t fg, uint32_t bg) {
    descriptor_defs::ColorPair colors{fg, bg};
    return descriptor_set_property(
               static_cast<uint32_t>(console),
               static_cast<uint32_t>(descriptor_defs::Property::ConsoleColor),
               &colors,
               sizeof(colors)) == 0;
}

void clear_console(long console) {
    descriptor_set_property(
        static_cast<uint32_t>(console),
        static_cast<uint32_t>(descriptor_defs::Property::ConsoleClear),
        nullptr,
        0);
}

void write_text(long console, const char* text) {
    if (text == nullptr) {
        return;
    }
    descriptor_write(static_cast<uint32_t>(console), text, strlen(text));
}

void write_span(long console, const char* text, size_t len) {
    descriptor_write(static_cast<uint32_t>(console), text, len);
}

void pad_to(long console, size_t used, uint32_t cols) {
    while (used < cols) {
        descriptor_write(static_cast<uint32_t>(console), " ", 1);
        ++used;
    }
}

void set_status(const char* text) {
    strlcpy(g_status, text, sizeof(g_status));
}

void append_dec(char* out, size_t out_size, uint64_t value) {
    size_t len = strlen(out);
    if (len + 1 >= out_size) {
        return;
    }
    char tmp[24];
    size_t pos = 0;
    if (value == 0) {
        tmp[pos++] = '0';
    } else {
        while (value != 0 && pos < sizeof(tmp)) {
            tmp[pos++] = static_cast<char>('0' + (value % 10u));
            value /= 10u;
        }
    }
    while (pos != 0 && len + 1 < out_size) {
        out[len++] = tmp[--pos];
    }
    out[len] = '\0';
}

size_t wrap_width(uint32_t cols) {
    return cols > 4 ? static_cast<size_t>(cols) : 4;
}

void wrap_segment(const char* line,
                  size_t start,
                  size_t width,
                  size_t& end,
                  size_t& next_start) {
    size_t len = strlen(line);
    if (len == 0 || start >= len) {
        end = len;
        next_start = len + 1;
        return;
    }

    size_t limit = start + width;
    if (limit >= len) {
        end = len;
        next_start = len + 1;
        return;
    }

    size_t break_at = limit;
    bool found_space = false;
    for (size_t i = limit; i > start; --i) {
        if (line[i] == ' ') {
            break_at = i;
            found_space = true;
            break;
        }
    }

    if (found_space) {
        end = break_at;
        next_start = break_at + 1;
        while (next_start < len && line[next_start] == ' ') {
            ++next_start;
        }
    } else {
        end = limit;
        next_start = limit;
    }
}

size_t visual_rows_for_line(const char* line, size_t width) {
    size_t len = strlen(line);
    if (len == 0) {
        return 1;
    }

    size_t rows = 0;
    size_t start = 0;
    while (start <= len) {
        size_t end = 0;
        size_t next = 0;
        wrap_segment(line, start, width, end, next);
        ++rows;
        if (next > len) {
            break;
        }
        start = next;
    }
    return rows;
}

size_t cursor_visual_position(size_t width, uint32_t& cursor_x) {
    size_t visual = 0;
    for (size_t line = 0; line < g_cursor_line; ++line) {
        visual += visual_rows_for_line(g_lines[line], width);
    }

    const char* text = g_lines[g_cursor_line];
    size_t len = strlen(text);
    size_t start = 0;
    while (true) {
        size_t end = 0;
        size_t next = 0;
        wrap_segment(text, start, width, end, next);
        if (g_cursor_col >= start &&
            (g_cursor_col <= end || next > len)) {
            size_t x = g_cursor_col > end ? end - start : g_cursor_col - start;
            if (x >= width) {
                x = width - 1;
            }
            cursor_x = static_cast<uint32_t>(x);
            return visual;
        }
        ++visual;
        if (next > len) {
            cursor_x = static_cast<uint32_t>(end >= start ? end - start : 0);
            return visual;
        }
        start = next;
    }
}

bool query_console_size(uint32_t& cols, uint32_t& rows) {
    long fb = framebuffer_open();
    if (fb < 0) {
        return false;
    }
    descriptor_defs::FramebufferInfo info{};
    bool ok = framebuffer_get_info(static_cast<uint32_t>(fb), &info) == 0;
    descriptor_close(static_cast<uint32_t>(fb));
    if (!ok) {
        return false;
    }
    if (info.width >= 16) {
        cols = info.width / 16;
    }
    if (info.height >= 22) {
        rows = info.height / 22;
    }
    return true;
}

bool save_document(const char* path) {
    file_remove(path);
    long file = file_create(path);
    if (file < 0) {
        return false;
    }
    for (size_t i = 0; i < g_line_count; ++i) {
        size_t len = strlen(g_lines[i]);
        if (len != 0 &&
            file_write(static_cast<uint32_t>(file), g_lines[i], len) < 0) {
            file_close(static_cast<uint32_t>(file));
            return false;
        }
        if (i + 1 < g_line_count) {
            file_write(static_cast<uint32_t>(file), "\n", 1);
        }
    }
    file_close(static_cast<uint32_t>(file));
    g_dirty = false;
    g_quit_armed = false;
    set_status("Saved");
    return true;
}

void load_document(const char* path) {
    long file = file_open(path);
    if (file < 0) {
        g_line_count = 1;
        g_lines[0][0] = '\0';
        set_status("New document");
        return;
    }

    g_line_count = 1;
    size_t line = 0;
    size_t col = 0;
    uint8_t buffer[512];
    while (true) {
        long read = file_read(static_cast<uint32_t>(file), buffer, sizeof(buffer));
        if (read <= 0) {
            break;
        }
        for (size_t i = 0; i < static_cast<size_t>(read); ++i) {
            char ch = static_cast<char>(buffer[i]);
            if (ch == '\r') {
                continue;
            }
            if (ch == '\n') {
                g_lines[line][col] = '\0';
                if (line + 1 < kMaxLines) {
                    ++line;
                    ++g_line_count;
                    col = 0;
                    g_lines[line][0] = '\0';
                }
                continue;
            }
            if (ch >= 0x20 && ch <= 0x7E && col + 1 < kLineCap) {
                g_lines[line][col++] = ch;
                g_lines[line][col] = '\0';
            }
        }
    }
    file_close(static_cast<uint32_t>(file));
    set_status("Loaded");
}

void ensure_cursor_visible(uint32_t rows, uint32_t cols) {
    size_t body_rows = rows > 3 ? rows - 3 : 1;
    uint32_t cursor_x = 0;
    size_t cursor_visual = cursor_visual_position(wrap_width(cols), cursor_x);
    if (cursor_visual < g_top_visual) {
        g_top_visual = cursor_visual;
    }
    if (cursor_visual >= g_top_visual + body_rows) {
        g_top_visual = cursor_visual - body_rows + 1;
    }
}

void render(long console, const char* path, uint32_t cols, uint32_t rows) {
    ensure_cursor_visible(rows, cols);
    clear_console(console);

    set_cursor(console, 0, 0);
    set_color(console, kBarFg, kBarBg);
    write_text(console, " Neutrino Write  ");
    write_text(console, path);
    if (g_dirty) {
        write_text(console, " *");
    }
    pad_to(console, 17 + strlen(path) + (g_dirty ? 2 : 0), cols);

    set_color(console, kDefaultFg, kDefaultBg);
    uint32_t body_rows = rows > 3 ? rows - 3 : 1;
    size_t width = wrap_width(cols);
    size_t visual = 0;
    uint32_t out_row = 0;
    for (size_t line_index = 0;
         line_index < g_line_count && out_row < body_rows;
         ++line_index) {
        const char* line = g_lines[line_index];
        size_t len = strlen(line);
        size_t start = 0;
        while (out_row < body_rows) {
            size_t end = 0;
            size_t next = 0;
            wrap_segment(line, start, width, end, next);
            if (visual >= g_top_visual) {
                set_cursor(console, 0, out_row + 1);
                size_t draw_len = end >= start ? end - start : 0;
                if (draw_len > cols) {
                    draw_len = cols;
                }
                write_span(console, line + start, draw_len);
                pad_to(console, draw_len, cols);
                ++out_row;
            }
            ++visual;
            if (next > len) {
                break;
            }
            start = next;
        }
    }
    while (out_row < body_rows) {
        set_cursor(console, 0, out_row + 1);
        pad_to(console, 0, cols);
        ++out_row;
    }

    set_cursor(console, 0, body_rows + 1);
    set_color(console, kAccentFg, kDefaultBg);
    write_text(console, "Ctrl+S Save  Ctrl+Q Quit  Enter New Line  Backspace Delete");
    pad_to(console, 60, cols);

    char stat[128];
    strlcpy(stat, "Ln ", sizeof(stat));
    append_dec(stat, sizeof(stat), g_cursor_line + 1);
    strlcpy(stat + strlen(stat), " Col ", sizeof(stat) - strlen(stat));
    append_dec(stat, sizeof(stat), g_cursor_col + 1);
    strlcpy(stat + strlen(stat), " | ", sizeof(stat) - strlen(stat));
    strlcpy(stat + strlen(stat), g_status, sizeof(stat) - strlen(stat));
    set_cursor(console, 0, body_rows + 2);
    set_color(console, kBarFg, kBarBg);
    write_text(console, stat);
    pad_to(console, strlen(stat), cols);

    set_color(console, kDefaultFg, kDefaultBg);
    uint32_t cursor_x = 0;
    size_t cursor_visual = cursor_visual_position(width, cursor_x);
    uint32_t y = 1;
    if (cursor_visual >= g_top_visual) {
        y = static_cast<uint32_t>(cursor_visual - g_top_visual + 1);
    }
    if (cursor_x >= cols) {
        cursor_x = cols - 1;
    }
    set_cursor(console, cursor_x, y);
}

void insert_char(char ch) {
    char* line = g_lines[g_cursor_line];
    size_t len = strlen(line);
    if (len + 1 >= kLineCap) {
        set_status("Line full");
        return;
    }
    memmove(line + g_cursor_col + 1, line + g_cursor_col, len - g_cursor_col + 1);
    line[g_cursor_col++] = ch;
    g_dirty = true;
    g_quit_armed = false;
    set_status("Editing");
}

void split_line() {
    if (g_line_count >= kMaxLines) {
        set_status("Document full");
        return;
    }
    for (size_t i = g_line_count; i > g_cursor_line + 1; --i) {
        memcpy(g_lines[i], g_lines[i - 1], kLineCap);
    }
    char* line = g_lines[g_cursor_line];
    strlcpy(g_lines[g_cursor_line + 1], line + g_cursor_col, kLineCap);
    line[g_cursor_col] = '\0';
    ++g_line_count;
    ++g_cursor_line;
    g_cursor_col = 0;
    g_dirty = true;
    g_quit_armed = false;
}

void backspace() {
    if (g_cursor_col > 0) {
        char* line = g_lines[g_cursor_line];
        size_t len = strlen(line);
        memmove(line + g_cursor_col - 1, line + g_cursor_col,
                len - g_cursor_col + 1);
        --g_cursor_col;
        g_dirty = true;
        g_quit_armed = false;
        return;
    }
    if (g_cursor_line == 0) {
        return;
    }
    size_t prev_len = strlen(g_lines[g_cursor_line - 1]);
    size_t cur_len = strlen(g_lines[g_cursor_line]);
    if (prev_len + cur_len + 1 >= kLineCap) {
        set_status("Previous line full");
        return;
    }
    strlcpy(g_lines[g_cursor_line - 1] + prev_len,
            g_lines[g_cursor_line],
            kLineCap - prev_len);
    for (size_t i = g_cursor_line; i + 1 < g_line_count; ++i) {
        memcpy(g_lines[i], g_lines[i + 1], kLineCap);
    }
    --g_line_count;
    --g_cursor_line;
    g_cursor_col = prev_len;
    g_dirty = true;
    g_quit_armed = false;
}

}  // namespace

int main(uint64_t arg_ptr, uint64_t) {
    const char* arg = reinterpret_cast<const char*>(arg_ptr);
    char path[kMaxPath];
    strlcpy(path, (arg && arg[0]) ? arg : "document.txt", sizeof(path));

    long console = descriptor_open(kDescConsole, 0);
    long keyboard = descriptor_open(kDescKeyboard, 0);
    if (console < 0 || keyboard < 0) {
        return 1;
    }

    load_document(path);
    uint32_t cols = 80;
    uint32_t rows = 25;
    query_console_size(cols, rows);
    render(console, path, cols, rows);

    while (true) {
        descriptor_defs::KeyboardEvent events[8]{};
        long read = descriptor_read(static_cast<uint32_t>(keyboard),
                                    events,
                                    sizeof(events));
        if (read <= 0) {
            yield();
            continue;
        }
        bool redraw = false;
        size_t count = static_cast<size_t>(read) / sizeof(events[0]);
        for (size_t i = 0; i < count; ++i) {
            const auto& ev = events[i];
            if (!keyboard::is_pressed(ev)) {
                continue;
            }
            int32_t dx = 0;
            int32_t dy = 0;
            if (keyboard::is_arrow_key(ev, dx, dy)) {
                if (dy < 0 && g_cursor_line > 0) {
                    --g_cursor_line;
                } else if (dy > 0 && g_cursor_line + 1 < g_line_count) {
                    ++g_cursor_line;
                }
                size_t len = strlen(g_lines[g_cursor_line]);
                if (dx < 0 && g_cursor_col > 0) {
                    --g_cursor_col;
                } else if (dx > 0 && g_cursor_col < len) {
                    ++g_cursor_col;
                }
                if (g_cursor_col > len) {
                    g_cursor_col = len;
                }
                redraw = true;
                continue;
            }
            char ch = keyboard::scancode_to_char(ev.scancode, ev.mods);
            bool ctrl = (ev.mods & descriptor_defs::kKeyboardModCtrl) != 0;
            if (ctrl && (ch == 's' || ch == 'S')) {
                if (!save_document(path)) {
                    set_status("Save failed");
                }
                redraw = true;
            } else if (ctrl && (ch == 'q' || ch == 'Q')) {
                if (g_dirty && !g_quit_armed) {
                    g_quit_armed = true;
                    set_status("Unsaved changes - Ctrl+Q again to quit");
                    redraw = true;
                } else {
                    clear_console(console);
                    return 0;
                }
            } else if (ch == '\n' || ch == '\r') {
                split_line();
                redraw = true;
            } else if (ch == '\b' || ch == 0x7F) {
                backspace();
                redraw = true;
            } else if (ch >= 0x20 && ch <= 0x7E) {
                insert_char(ch);
                redraw = true;
            }
        }
        if (redraw) {
            render(console, path, cols, rows);
        }
    }
}
