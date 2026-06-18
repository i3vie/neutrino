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
constexpr uint32_t kBarBg = 0xFFA8E6A3u;
constexpr uint32_t kCellFg = 0xFF101820u;
constexpr uint32_t kCellBg = 0xFFFFD27Du;
constexpr size_t kRows = 100;
constexpr size_t kCols = 26;
constexpr size_t kCellCap = 32;
constexpr size_t kCellWidth = 10;
constexpr size_t kMaxPath = 128;

char g_cells[kRows][kCols][kCellCap];
size_t g_row = 0;
size_t g_col = 0;
size_t g_top_row = 0;
size_t g_left_col = 0;
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
    if (text) {
        descriptor_write(static_cast<uint32_t>(console), text, strlen(text));
    }
}

void write_span(long console, const char* text, size_t len) {
    descriptor_write(static_cast<uint32_t>(console), text, len);
}

void pad(long console, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        descriptor_write(static_cast<uint32_t>(console), " ", 1);
    }
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

void append_dec(char* out, size_t out_size, int64_t value) {
    size_t len = strlen(out);
    if (len + 1 >= out_size) {
        return;
    }
    if (value < 0) {
        out[len++] = '-';
        out[len] = '\0';
        value = -value;
    }
    char tmp[24];
    size_t pos = 0;
    uint64_t unsigned_value = static_cast<uint64_t>(value);
    if (unsigned_value == 0) {
        tmp[pos++] = '0';
    } else {
        while (unsigned_value != 0 && pos < sizeof(tmp)) {
            tmp[pos++] = static_cast<char>('0' + (unsigned_value % 10u));
            unsigned_value /= 10u;
        }
    }
    while (pos != 0 && len + 1 < out_size) {
        out[len++] = tmp[--pos];
    }
    out[len] = '\0';
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

bool parse_cell_ref(const char*& text, size_t& row, size_t& col) {
    if (*text < 'A' || *text > 'Z') {
        return false;
    }
    col = static_cast<size_t>(*text - 'A');
    ++text;
    if (*text < '1' || *text > '9') {
        return false;
    }
    size_t value = 0;
    while (*text >= '0' && *text <= '9') {
        value = value * 10 + static_cast<size_t>(*text - '0');
        ++text;
    }
    if (value == 0 || value > kRows) {
        return false;
    }
    row = value - 1;
    return true;
}

bool parse_number(const char*& text, int64_t& out) {
    bool negative = false;
    if (*text == '-') {
        negative = true;
        ++text;
    }
    if (*text < '0' || *text > '9') {
        return false;
    }
    int64_t value = 0;
    while (*text >= '0' && *text <= '9') {
        value = value * 10 + static_cast<int64_t>(*text - '0');
        ++text;
    }
    out = negative ? -value : value;
    return true;
}

int64_t cell_value(size_t row, size_t col, uint32_t depth);

bool parse_term(const char*& text, int64_t& out, uint32_t depth) {
    size_t ref_row = 0;
    size_t ref_col = 0;
    if (parse_cell_ref(text, ref_row, ref_col)) {
        out = cell_value(ref_row, ref_col, depth + 1);
        return true;
    }
    return parse_number(text, out);
}

int64_t eval_formula(const char* text, uint32_t depth) {
    if (depth > 8 || text == nullptr || text[0] != '=') {
        return 0;
    }
    ++text;
    int64_t left = 0;
    if (!parse_term(text, left, depth)) {
        return 0;
    }
    if (*text == '\0') {
        return left;
    }
    char op = *text++;
    int64_t right = 0;
    if (!parse_term(text, right, depth)) {
        return left;
    }
    if (op == '+') return left + right;
    if (op == '-') return left - right;
    if (op == '*') return left * right;
    if (op == '/' && right != 0) return left / right;
    return left;
}

int64_t cell_value(size_t row, size_t col, uint32_t depth) {
    const char* text = g_cells[row][col];
    if (text[0] == '=') {
        return eval_formula(text, depth);
    }
    const char* cursor = text;
    int64_t value = 0;
    return parse_number(cursor, value) ? value : 0;
}

void cell_display(size_t row, size_t col, char* out, size_t out_size) {
    const char* text = g_cells[row][col];
    if (text[0] == '=') {
        out[0] = '\0';
        append_dec(out, out_size, cell_value(row, col, 0));
    } else {
        strlcpy(out, text, out_size);
    }
}

bool save_sheet(const char* path) {
    file_remove(path);
    long file = file_create(path);
    if (file < 0) {
        return false;
    }
    size_t last_row = 0;
    for (size_t r = 0; r < kRows; ++r) {
        for (size_t c = 0; c < kCols; ++c) {
            if (g_cells[r][c][0] != '\0') {
                last_row = r;
            }
        }
    }
    for (size_t r = 0; r <= last_row; ++r) {
        size_t last_col = 0;
        for (size_t c = 0; c < kCols; ++c) {
            if (g_cells[r][c][0] != '\0') {
                last_col = c;
            }
        }
        for (size_t c = 0; c <= last_col; ++c) {
            if (c != 0) {
                file_write(static_cast<uint32_t>(file), ",", 1);
            }
            size_t len = strlen(g_cells[r][c]);
            if (len != 0) {
                file_write(static_cast<uint32_t>(file), g_cells[r][c], len);
            }
        }
        if (r != last_row) {
            file_write(static_cast<uint32_t>(file), "\n", 1);
        }
    }
    file_close(static_cast<uint32_t>(file));
    g_dirty = false;
    g_quit_armed = false;
    set_status("Saved");
    return true;
}

void load_sheet(const char* path) {
    long file = file_open(path);
    if (file < 0) {
        set_status("New sheet");
        return;
    }
    size_t row = 0;
    size_t col = 0;
    size_t len = 0;
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
            if (ch == ',' || ch == '\n') {
                g_cells[row][col][len] = '\0';
                len = 0;
                if (ch == ',') {
                    if (col + 1 < kCols) ++col;
                } else {
                    if (row + 1 < kRows) ++row;
                    col = 0;
                }
                continue;
            }
            if (ch >= 0x20 && ch <= 0x7E && len + 1 < kCellCap) {
                g_cells[row][col][len++] = ch;
                g_cells[row][col][len] = '\0';
            }
        }
    }
    file_close(static_cast<uint32_t>(file));
    set_status("Loaded");
}

void ensure_visible(uint32_t rows, uint32_t cols) {
    size_t visible_rows = rows > 5 ? rows - 5 : 1;
    size_t visible_cols = cols > 5 ? (cols - 5) / kCellWidth : 1;
    if (visible_cols == 0) visible_cols = 1;
    if (g_row < g_top_row) g_top_row = g_row;
    if (g_row >= g_top_row + visible_rows) g_top_row = g_row - visible_rows + 1;
    if (g_col < g_left_col) g_left_col = g_col;
    if (g_col >= g_left_col + visible_cols) g_left_col = g_col - visible_cols + 1;
}

void render(long console, const char* path, uint32_t cols, uint32_t rows) {
    ensure_visible(rows, cols);
    clear_console(console);
    set_cursor(console, 0, 0);
    set_color(console, kBarFg, kBarBg);
    write_text(console, " Neutrino Sheet  ");
    write_text(console, path);
    if (g_dirty) write_text(console, " *");
    pad_to(console, 17 + strlen(path) + (g_dirty ? 2 : 0), cols);

    size_t visible_rows = rows > 5 ? rows - 5 : 1;
    size_t visible_cols = cols > 5 ? (cols - 5) / kCellWidth : 1;
    if (visible_cols > kCols - g_left_col) visible_cols = kCols - g_left_col;

    set_color(console, kDefaultFg, kDefaultBg);
    set_cursor(console, 0, 1);
    write_text(console, "    ");
    for (size_t c = 0; c < visible_cols; ++c) {
        char head[4] = {' ', static_cast<char>('A' + g_left_col + c), ' ', '\0'};
        write_text(console, head);
        pad(console, kCellWidth - 3);
    }
    pad_to(console, 4 + visible_cols * kCellWidth, cols);

    for (size_t r = 0; r < visible_rows; ++r) {
        size_t real_row = g_top_row + r;
        set_cursor(console, 0, static_cast<uint32_t>(2 + r));
        char row_head[8] = "";
        append_dec(row_head, sizeof(row_head), static_cast<int64_t>(real_row + 1));
        write_text(console, row_head);
        pad(console, 4 - (strlen(row_head) < 4 ? strlen(row_head) : 4));
        for (size_t c = 0; c < visible_cols; ++c) {
            size_t real_col = g_left_col + c;
            bool selected = real_row == g_row && real_col == g_col;
            if (selected) {
                set_color(console, kCellFg, kCellBg);
            }
            char display[32];
            cell_display(real_row, real_col, display, sizeof(display));
            size_t len = strlen(display);
            if (len > kCellWidth - 1) len = kCellWidth - 1;
            write_span(console, display, len);
            pad(console, kCellWidth - len);
            if (selected) {
                set_color(console, kDefaultFg, kDefaultBg);
            }
        }
        pad_to(console, 4 + visible_cols * kCellWidth, cols);
    }

    char formula[96];
    formula[0] = static_cast<char>('A' + g_col);
    formula[1] = '\0';
    append_dec(formula, sizeof(formula), static_cast<int64_t>(g_row + 1));
    strlcpy(formula + strlen(formula), ": ", sizeof(formula) - strlen(formula));
    strlcpy(formula + strlen(formula), g_cells[g_row][g_col],
            sizeof(formula) - strlen(formula));
    set_cursor(console, 0, static_cast<uint32_t>(2 + visible_rows));
    set_color(console, kBarFg, kBarBg);
    write_text(console, formula);
    pad_to(console, strlen(formula), cols);

    set_cursor(console, 0, static_cast<uint32_t>(3 + visible_rows));
    set_color(console, kDefaultFg, kDefaultBg);
    write_text(console, "Enter Edit  Ctrl+S Save  Ctrl+Q Quit  Formulas: =A1+B2");
    pad_to(console, 58, cols);

    set_cursor(console, 0, static_cast<uint32_t>(4 + visible_rows));
    set_color(console, kBarFg, kBarBg);
    write_text(console, g_status);
    pad_to(console, strlen(g_status), cols);
    set_color(console, kDefaultFg, kDefaultBg);
}

void edit_cell(uint32_t keyboard, long console, uint32_t cols, uint32_t y) {
    char input[kCellCap];
    strlcpy(input, g_cells[g_row][g_col], sizeof(input));
    size_t len = strlen(input);
    for (;;) {
        set_cursor(console, 0, y);
        set_color(console, kBarFg, kBarBg);
        write_text(console, "Edit: ");
        write_text(console, input);
        pad_to(console, 6 + strlen(input), cols);
        descriptor_defs::KeyboardEvent events[8]{};
        long read = descriptor_read(keyboard, events, sizeof(events));
        if (read <= 0) {
            yield();
            continue;
        }
        size_t count = static_cast<size_t>(read) / sizeof(events[0]);
        for (size_t i = 0; i < count; ++i) {
            const auto& ev = events[i];
            if (!keyboard::is_pressed(ev) || keyboard::is_extended(ev)) {
                continue;
            }
            char ch = keyboard::scancode_to_char(ev.scancode, ev.mods);
            if (ch == '\n' || ch == '\r') {
                strlcpy(g_cells[g_row][g_col], input, kCellCap);
                g_dirty = true;
                g_quit_armed = false;
                set_status("Editing");
                return;
            }
            if (ch == 27) {
                set_status("Canceled");
                return;
            }
            if (ch == '\b' || ch == 0x7F) {
                if (len > 0) input[--len] = '\0';
            } else if (ch >= 0x20 && ch <= 0x7E && len + 1 < sizeof(input)) {
                input[len++] = ch;
                input[len] = '\0';
            }
        }
    }
}

}  // namespace

int main(uint64_t arg_ptr, uint64_t) {
    const char* arg = reinterpret_cast<const char*>(arg_ptr);
    char path[kMaxPath];
    strlcpy(path, (arg && arg[0]) ? arg : "sheet.csv", sizeof(path));

    long console = descriptor_open(kDescConsole, 0);
    long keyboard = descriptor_open(kDescKeyboard, 0);
    if (console < 0 || keyboard < 0) {
        return 1;
    }

    load_sheet(path);
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
                if (dx < 0 && g_col > 0) --g_col;
                if (dx > 0 && g_col + 1 < kCols) ++g_col;
                if (dy < 0 && g_row > 0) --g_row;
                if (dy > 0 && g_row + 1 < kRows) ++g_row;
                redraw = true;
                continue;
            }
            if (keyboard::is_extended(ev)) {
                continue;
            }
            char ch = keyboard::scancode_to_char(ev.scancode, ev.mods);
            bool ctrl = (ev.mods & descriptor_defs::kKeyboardModCtrl) != 0;
            if (ctrl && (ch == 's' || ch == 'S')) {
                if (!save_sheet(path)) {
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
                size_t visible_rows = rows > 5 ? rows - 5 : 1;
                edit_cell(static_cast<uint32_t>(keyboard),
                          console,
                          cols,
                          static_cast<uint32_t>(4 + visible_rows));
                redraw = true;
            } else if (ch >= 0x20 && ch <= 0x7E) {
                g_cells[g_row][g_col][0] = ch;
                g_cells[g_row][g_col][1] = '\0';
                size_t visible_rows = rows > 5 ? rows - 5 : 1;
                edit_cell(static_cast<uint32_t>(keyboard),
                          console,
                          cols,
                          static_cast<uint32_t>(4 + visible_rows));
                redraw = true;
            }
        }
        if (redraw) {
            render(console, path, cols, rows);
        }
    }
}
