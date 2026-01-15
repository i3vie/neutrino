#include <stddef.h>
#include <stdint.h>

#include "descriptors.hpp"
#include "font8x8_basic.hpp"
#include "wm_protocol.hpp"
#include "lattice/lattice.hpp"
#include "../crt/syscall.hpp"

namespace {

constexpr const char kRegistryName[] = "wm.registry";
constexpr const char kShellPath[] = "binary/shell.elf";
constexpr uint32_t kFontWidth = 8;
constexpr uint32_t kFontHeight = 8;
constexpr uint32_t kBaseCursorHeight = 2;
constexpr uint32_t kDefaultScale = 1;
constexpr uint32_t kMaxScale = 4;
constexpr uint32_t kDefaultLineGap = 2;
constexpr uint32_t kMaxLineGap = 6;
constexpr uint32_t kTextPaddingX = 6;

struct Terminal {
    uint8_t* buffer;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t bytes_per_pixel;
    uint32_t cols;
    uint32_t rows;
    uint32_t scale;
    uint32_t cell_width;
    uint32_t cell_height;
    uint32_t glyph_width;
    uint32_t glyph_height;
    uint32_t cursor_height;
    uint32_t padding_x;
    uint32_t fg;
    uint32_t bg;
    uint32_t cursor;
};

size_t str_len(const char* text) {
    size_t len = 0;
    if (text == nullptr) {
        return 0;
    }
    while (text[len] != '\0') {
        ++len;
    }
    return len;
}

void copy_string(char* dest, size_t dest_size, const char* src) {
    if (dest == nullptr || dest_size == 0) {
        return;
    }
    size_t i = 0;
    if (src != nullptr) {
        for (; i + 1 < dest_size && src[i] != '\0'; ++i) {
            dest[i] = src[i];
        }
    }
    dest[i] = '\0';
}

void extract_mount_name(const char* path, char* out, size_t out_size) {
    if (out == nullptr || out_size == 0) {
        return;
    }
    out[0] = '\0';
    if (path == nullptr || path[0] != '/') {
        return;
    }
    size_t src = 1;
    size_t dst = 0;
    while (path[src] != '\0' && path[src] != '/') {
        if (dst + 1 >= out_size) {
            out[0] = '\0';
            return;
        }
        out[dst++] = path[src++];
    }
    if (dst == 0) {
        out[0] = '\0';
        return;
    }
    out[dst] = '\0';
}

bool build_mount_subpath(const char* mount,
                         const char* suffix,
                         char* out,
                         size_t out_size) {
    if (out == nullptr || out_size == 0) {
        return false;
    }
    if (mount == nullptr || mount[0] == '\0') {
        return false;
    }
    size_t idx = 0;
    out[idx++] = '/';
    for (size_t i = 0; mount[i] != '\0'; ++i) {
        if (idx + 1 >= out_size) {
            return false;
        }
        out[idx++] = mount[i];
    }
    if (suffix != nullptr && suffix[0] != '\0') {
        if (idx + 1 >= out_size) {
            return false;
        }
        out[idx++] = '/';
        for (size_t i = 0; suffix[i] != '\0'; ++i) {
            if (idx + 1 >= out_size) {
                return false;
            }
            out[idx++] = suffix[i];
        }
    }
    out[idx] = '\0';
    return true;
}

bool is_space(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

const char* skip_spaces(const char* text) {
    if (text == nullptr) {
        return nullptr;
    }
    while (*text != '\0' && is_space(*text)) {
        ++text;
    }
    return text;
}

uint32_t parse_uint32(const char* text) {
    if (text == nullptr) {
        return 0;
    }
    uint32_t value = 0;
    while (*text >= '0' && *text <= '9') {
        uint32_t digit = static_cast<uint32_t>(*text - '0');
        value = value * 10 + digit;
        ++text;
    }
    return value;
}

bool token_has_prefix(const char* token, size_t token_len, const char* prefix) {
    if (token == nullptr || prefix == nullptr) {
        return false;
    }
    size_t idx = 0;
    while (prefix[idx] != '\0') {
        if (idx >= token_len || token[idx] != prefix[idx]) {
            return false;
        }
        ++idx;
    }
    return idx < token_len;
}

void parse_terminal_args(const char* args, uint32_t& scale, uint32_t& line_gap) {
    if (args == nullptr || args[0] == '\0') {
        return;
    }
    const char* cursor = args;
    while (cursor != nullptr && *cursor != '\0') {
        cursor = skip_spaces(cursor);
        if (cursor == nullptr || *cursor == '\0') {
            break;
        }
        const char* token = cursor;
        while (*cursor != '\0' && !is_space(*cursor)) {
            ++cursor;
        }
        size_t token_len = static_cast<size_t>(cursor - token);
        if (token_has_prefix(token, token_len, "scale=")) {
            scale = parse_uint32(token + 6);
        } else if (token_has_prefix(token, token_len, "gap=")) {
            line_gap = parse_uint32(token + 4);
        } else if (token_has_prefix(token, token_len, "linegap=")) {
            line_gap = parse_uint32(token + 8);
        } else if (token_has_prefix(token, token_len, "line_gap=")) {
            line_gap = parse_uint32(token + 9);
        }
    }
}

void clear_screen(const Terminal& term) {
    if (term.buffer == nullptr || term.width == 0 || term.height == 0) {
        return;
    }
    uint8_t pixel_bytes[4] = {0, 0, 0, 0};
    lattice::store_pixel(pixel_bytes, term.bytes_per_pixel, term.bg);
    for (uint32_t y = 0; y < term.height; ++y) {
        uint8_t* row = term.buffer + static_cast<size_t>(y) * term.stride;
        for (uint32_t x = 0; x < term.width; ++x) {
            size_t offset = static_cast<size_t>(x) * term.bytes_per_pixel;
            for (uint32_t byte = 0; byte < term.bytes_per_pixel; ++byte) {
                row[offset + byte] = pixel_bytes[byte];
            }
        }
    }
}

void draw_glyph(const Terminal& term,
                uint32_t cell_x,
                uint32_t cell_y,
                char ch) {
    if (term.buffer == nullptr) {
        return;
    }
    if (cell_x >= term.cols || cell_y >= term.rows) {
        return;
    }
    uint8_t uc = static_cast<uint8_t>(ch);
    if (uc >= 128) {
        uc = static_cast<uint8_t>('?');
    }
    uint32_t base_x = term.padding_x + cell_x * term.cell_width;
    uint32_t base_y = cell_y * term.cell_height;
    uint32_t scale = term.scale;
    for (uint32_t row = 0; row < kFontHeight; ++row) {
        uint8_t bits = font8x8_basic[uc][row];
        for (uint32_t sy = 0; sy < scale; ++sy) {
            uint32_t py = base_y + row * scale + sy;
            if (py >= term.height) {
                break;
            }
            for (uint32_t col = 0; col < kFontWidth; ++col) {
                bool on = (bits & (1u << col)) != 0;
                for (uint32_t sx = 0; sx < scale; ++sx) {
                    uint32_t px = base_x + col * scale + sx;
                    if (px >= term.width) {
                        break;
                    }
                    lattice::write_pixel(term.buffer,
                                         term.stride,
                                         term.bytes_per_pixel,
                                         px,
                                         py,
                                         on ? term.fg : term.bg);
                }
            }
        }
    }
}

void draw_cursor(const Terminal& term, uint32_t cell_x, uint32_t cell_y) {
    if (term.buffer == nullptr) {
        return;
    }
    if (cell_x >= term.cols || cell_y >= term.rows) {
        return;
    }
    uint32_t base_x = term.padding_x + cell_x * term.cell_width;
    uint32_t base_y = cell_y * term.cell_height;
    uint32_t cursor_height = term.cursor_height;
    if (cursor_height > term.glyph_height) {
        cursor_height = term.glyph_height;
    }
    uint32_t start_y = base_y + term.glyph_height - cursor_height;
    for (uint32_t row = 0; row < cursor_height; ++row) {
        uint32_t py = start_y + row;
        if (py >= term.height) {
            continue;
        }
        for (uint32_t col = 0; col < term.glyph_width; ++col) {
            uint32_t px = base_x + col;
            if (px >= term.width) {
                break;
            }
            lattice::write_pixel(term.buffer,
                                 term.stride,
                                 term.bytes_per_pixel,
                                 px,
                                 py,
                                 term.cursor);
        }
    }
}

bool cells_equal(const descriptor_defs::VtyCell& lhs,
                 const descriptor_defs::VtyCell& rhs) {
    return lhs.ch == rhs.ch &&
           lhs.fg == rhs.fg &&
           lhs.bg == rhs.bg &&
           lhs.flags == rhs.flags;
}

bool update_vty(const Terminal& term,
                const descriptor_defs::VtyCell* cells,
                descriptor_defs::VtyCell* prev_cells,
                uint32_t cursor_x,
                uint32_t cursor_y,
                uint32_t& prev_cursor_x,
                uint32_t& prev_cursor_y,
                bool& has_prev) {
    if (cells == nullptr || prev_cells == nullptr) {
        return false;
    }
    if (term.cols == 0 || term.rows == 0) {
        return false;
    }
    if (!has_prev) {
        clear_screen(term);
        for (uint32_t row = 0; row < term.rows; ++row) {
            size_t base = static_cast<size_t>(row) * term.cols;
            for (uint32_t col = 0; col < term.cols; ++col) {
                size_t idx = base + col;
                const auto& cell = cells[idx];
                draw_glyph(term, col, row, static_cast<char>(cell.ch));
                prev_cells[idx] = cell;
            }
        }
        draw_cursor(term, cursor_x, cursor_y);
        prev_cursor_x = cursor_x;
        prev_cursor_y = cursor_y;
        has_prev = true;
        return true;
    }

    bool changed = false;
    bool cursor_moved = (prev_cursor_x != cursor_x ||
                         prev_cursor_y != cursor_y);

    if (prev_cursor_x < term.cols &&
        prev_cursor_y < term.rows &&
        cursor_moved) {
        const auto& cell = cells[prev_cursor_y * term.cols + prev_cursor_x];
        draw_glyph(term, prev_cursor_x, prev_cursor_y, static_cast<char>(cell.ch));
        changed = true;
    }

    for (uint32_t row = 0; row < term.rows; ++row) {
        size_t base = static_cast<size_t>(row) * term.cols;
        for (uint32_t col = 0; col < term.cols; ++col) {
            size_t idx = base + col;
            const auto& cell = cells[idx];
            if (!cells_equal(cell, prev_cells[idx])) {
                draw_glyph(term, col, row, static_cast<char>(cell.ch));
                prev_cells[idx] = cell;
                changed = true;
            }
        }
    }

    if (cursor_moved || changed) {
        draw_cursor(term, cursor_x, cursor_y);
        prev_cursor_x = cursor_x;
        prev_cursor_y = cursor_y;
        changed = true;
    }
    return changed;
}

bool write_pipe_all(uint32_t handle, const void* data, size_t length) {
    if (data == nullptr || length == 0) {
        return false;
    }
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(data);
    size_t offset = 0;
    while (offset < length) {
        long written = descriptor_write(handle, bytes + offset, length - offset);
        if (written <= 0) {
            return false;
        }
        offset += static_cast<size_t>(written);
    }
    return true;
}

bool read_pipe_exact(uint32_t handle, void* data, size_t length) {
    if (data == nullptr || length == 0) {
        return false;
    }
    uint8_t* bytes = reinterpret_cast<uint8_t*>(data);
    size_t offset = 0;
    while (offset < length) {
        long read = descriptor_read(handle, bytes + offset, length - offset);
        if (read < 0) {
            return false;
        }
        if (read == 0) {
            yield();
            continue;
        }
        offset += static_cast<size_t>(read);
    }
    return true;
}

bool open_registry(uint32_t& handle, wm::Registry*& registry) {
    long shm = shared_memory_open(kRegistryName, sizeof(wm::Registry));
    if (shm < 0) {
        return false;
    }
    descriptor_defs::SharedMemoryInfo info{};
    if (shared_memory_get_info(static_cast<uint32_t>(shm), &info) != 0 ||
        info.base == 0 || info.length < sizeof(wm::Registry)) {
        descriptor_close(static_cast<uint32_t>(shm));
        return false;
    }
    handle = static_cast<uint32_t>(shm);
    registry = reinterpret_cast<wm::Registry*>(info.base);
    return true;
}

void uint32_to_string(uint32_t value, char* buffer, size_t buffer_size) {
    if (buffer == nullptr || buffer_size == 0) {
        return;
    }
    char tmp[16];
    size_t pos = 0;
    if (value == 0) {
        tmp[pos++] = '0';
    } else {
        while (value > 0 && pos < sizeof(tmp)) {
            tmp[pos++] = static_cast<char>('0' + (value % 10));
            value /= 10;
        }
    }
    size_t idx = 0;
    while (idx + 1 < buffer_size && pos > 0) {
        buffer[idx++] = tmp[--pos];
    }
    buffer[idx] = '\0';
}

}  // namespace

int main(uint64_t arg, uint64_t) {
    const char* args = reinterpret_cast<const char*>(arg);
    uint32_t scale = kDefaultScale;
    uint32_t line_gap = kDefaultLineGap;
    parse_terminal_args(args, scale, line_gap);
    if (scale == 0) {
        scale = 1;
    }
    if (scale > kMaxScale) {
        scale = kMaxScale;
    }
    if (line_gap > kMaxLineGap) {
        line_gap = kMaxLineGap;
    }

    uint64_t vty_flags =
        static_cast<uint64_t>(descriptor_defs::Flag::Readable) |
        static_cast<uint64_t>(descriptor_defs::Flag::Writable);
    long vty_handle =
        descriptor_open(static_cast<uint32_t>(descriptor_defs::Type::Vty),
                        0,
                        vty_flags,
                        0);
    if (vty_handle < 0) {
        return 1;
    }
    descriptor_defs::VtyInfo vty_info{};
    if (descriptor_get_property(static_cast<uint32_t>(vty_handle),
                                static_cast<uint32_t>(descriptor_defs::Property::VtyInfo),
                                &vty_info,
                                sizeof(vty_info)) != 0) {
        return 1;
    }
    uint32_t cols = (vty_info.cols != 0) ? vty_info.cols : 80;
    uint32_t rows = (vty_info.rows != 0) ? vty_info.rows : 25;

    uint32_t glyph_width = kFontWidth * scale;
    uint32_t glyph_height = kFontHeight * scale;
    uint32_t cell_width = glyph_width;
    uint32_t cell_height = glyph_height + line_gap;
    uint32_t cursor_height = kBaseCursorHeight * scale;
    if (cursor_height == 0) {
        cursor_height = 1;
    }

    uint32_t registry_handle = 0;
    wm::Registry* registry = nullptr;
    if (!open_registry(registry_handle, registry)) {
        return 1;
    }

    while (registry->magic != wm::kRegistryMagic ||
           registry->version != wm::kRegistryVersion ||
           registry->server_pipe_id == 0) {
        yield();
    }
    uint32_t server_pipe_id = registry->server_pipe_id;
    descriptor_close(registry_handle);

    uint64_t reply_flags =
        static_cast<uint64_t>(descriptor_defs::Flag::Readable) |
        static_cast<uint64_t>(descriptor_defs::Flag::Async);
    long reply_handle = pipe_open_new(reply_flags);
    if (reply_handle < 0) {
        return 1;
    }
    descriptor_defs::PipeInfo reply_info{};
    if (pipe_get_info(static_cast<uint32_t>(reply_handle), &reply_info) != 0 ||
        reply_info.id == 0) {
        descriptor_close(static_cast<uint32_t>(reply_handle));
        return 1;
    }

    uint64_t server_flags =
        static_cast<uint64_t>(descriptor_defs::Flag::Writable) |
        static_cast<uint64_t>(descriptor_defs::Flag::Async);
    long server_handle =
        pipe_open_existing(server_flags, server_pipe_id);
    if (server_handle < 0) {
        descriptor_close(static_cast<uint32_t>(reply_handle));
        return 1;
    }

    uint32_t request_width = cols * cell_width + kTextPaddingX;
    uint32_t request_height = 0;
    if (rows > 0) {
        request_height = rows * glyph_height + (rows - 1) * line_gap;
    }

    wm::CreateRequest request{};
    request.type = static_cast<uint32_t>(wm::MessageType::CreateWindow);
    request.reply_pipe_id = reply_info.id;
    request.width = request_width;
    request.height = request_height;
    request.flags = 0;
    copy_string(request.title, sizeof(request.title), "Ion");

    if (!write_pipe_all(static_cast<uint32_t>(server_handle),
                        &request,
                        sizeof(request))) {
        descriptor_close(static_cast<uint32_t>(server_handle));
        descriptor_close(static_cast<uint32_t>(reply_handle));
        return 1;
    }
    descriptor_close(static_cast<uint32_t>(server_handle));

    wm::CreateResponse response{};
    if (!read_pipe_exact(static_cast<uint32_t>(reply_handle),
                         &response,
                         sizeof(response)) ||
        response.status != 0) {
        descriptor_close(static_cast<uint32_t>(reply_handle));
        return 1;
    }

    uint32_t present_handle = kInvalidDescriptor;
    if (response.out_pipe_id != 0) {
        uint64_t present_flags =
            static_cast<uint64_t>(descriptor_defs::Flag::Writable) |
            static_cast<uint64_t>(descriptor_defs::Flag::Async);
        long handle = pipe_open_existing(present_flags, response.out_pipe_id);
        if (handle >= 0) {
            present_handle = static_cast<uint32_t>(handle);
        }
    }

    char shm_name[sizeof(response.shm_name) + 1];
    for (size_t i = 0; i < sizeof(response.shm_name); ++i) {
        shm_name[i] = response.shm_name[i];
        if (response.shm_name[i] == '\0') {
            break;
        }
    }
    shm_name[sizeof(response.shm_name)] = '\0';

    long shm_handle = shared_memory_open(shm_name, 0);
    if (shm_handle < 0) {
        descriptor_close(static_cast<uint32_t>(reply_handle));
        return 1;
    }
    descriptor_defs::SharedMemoryInfo shm_info{};
    if (shared_memory_get_info(static_cast<uint32_t>(shm_handle), &shm_info) != 0 ||
        shm_info.base == 0 || shm_info.length == 0) {
        descriptor_close(static_cast<uint32_t>(shm_handle));
        descriptor_close(static_cast<uint32_t>(reply_handle));
        return 1;
    }

    uint32_t bytes_per_pixel = (response.format.bpp + 7u) / 8u;
    if (bytes_per_pixel == 0 || bytes_per_pixel > 4) {
        descriptor_close(static_cast<uint32_t>(shm_handle));
        descriptor_close(static_cast<uint32_t>(reply_handle));
        return 1;
    }

    Terminal term{};
    term.buffer = reinterpret_cast<uint8_t*>(shm_info.base);
    term.width = response.width;
    term.height = response.height;
    term.stride = response.stride;
    if (term.stride == 0) {
        term.stride = term.width * bytes_per_pixel;
    }
    term.bytes_per_pixel = bytes_per_pixel;
    term.cols = cols;
    term.rows = rows;
    term.scale = scale;
    term.cell_width = cell_width;
    term.cell_height = cell_height;
    term.glyph_width = glyph_width;
    term.glyph_height = glyph_height;
    term.cursor_height = cursor_height;
    term.padding_x = kTextPaddingX;
    term.fg = lattice::pack_color(response.format, 230, 230, 230);
    term.bg = lattice::pack_color(response.format, 16, 18, 24);
    term.cursor = lattice::pack_color(response.format, 128, 220, 128);

    size_t cell_bytes =
        static_cast<size_t>(cols) * rows * sizeof(descriptor_defs::VtyCell);
    auto* cell_buffer = static_cast<descriptor_defs::VtyCell*>(
        map_anonymous(cell_bytes, MAP_WRITE));
    if (cell_buffer == nullptr) {
        descriptor_close(static_cast<uint32_t>(shm_handle));
        descriptor_close(static_cast<uint32_t>(reply_handle));
        return 1;
    }
    auto* prev_cells = static_cast<descriptor_defs::VtyCell*>(
        map_anonymous(cell_bytes, MAP_WRITE));
    if (prev_cells == nullptr) {
        descriptor_close(static_cast<uint32_t>(shm_handle));
        descriptor_close(static_cast<uint32_t>(reply_handle));
        return 1;
    }
    bool has_prev = false;
    uint32_t prev_cursor_x = term.cols;
    uint32_t prev_cursor_y = term.rows;

    char args_buffer[32];
    copy_string(args_buffer, sizeof(args_buffer), "vty=");
    char id_buffer[16];
    uint32_to_string(vty_info.id, id_buffer, sizeof(id_buffer));
    size_t prefix_len = str_len(args_buffer);
    size_t id_len = str_len(id_buffer);
    if (prefix_len + id_len + 1 < sizeof(args_buffer)) {
        for (size_t i = 0; i < id_len; ++i) {
            args_buffer[prefix_len + i] = id_buffer[i];
        }
        args_buffer[prefix_len + id_len] = '\0';
    }

    char shell_path_buffer[128];
    const char* shell_path = kShellPath;
    char cwd_buffer[128];
    long cwd_len = getcwd(cwd_buffer, sizeof(cwd_buffer));
    if (cwd_len > 0 && cwd_buffer[0] != '\0') {
        char mount_name[64];
        extract_mount_name(cwd_buffer, mount_name, sizeof(mount_name));
        if (mount_name[0] != '\0' &&
            build_mount_subpath(mount_name,
                                "binary/shell.elf",
                                shell_path_buffer,
                                sizeof(shell_path_buffer))) {
            shell_path = shell_path_buffer;
        }
    }

    child(shell_path, args_buffer, 0, nullptr);

    while (1) {
        uint8_t key = 0;
        long read = descriptor_read(static_cast<uint32_t>(reply_handle),
                                    &key,
                                    1);
        if (read > 0) {
            if (key == static_cast<uint8_t>(wm::ServerMessage::Close)) {
                return 0;
            }
            descriptor_set_property(
                static_cast<uint32_t>(vty_handle),
                static_cast<uint32_t>(descriptor_defs::Property::VtyInjectInput),
                &key,
                1);
        }

        if (descriptor_get_property(
                static_cast<uint32_t>(vty_handle),
                static_cast<uint32_t>(descriptor_defs::Property::VtyInfo),
                &vty_info,
                sizeof(vty_info)) == 0) {
            if (descriptor_get_property(
                    static_cast<uint32_t>(vty_handle),
                    static_cast<uint32_t>(descriptor_defs::Property::VtyCells),
                    cell_buffer,
                    cell_bytes) == 0) {
                bool changed = update_vty(term,
                                          cell_buffer,
                                          prev_cells,
                                          vty_info.cursor_x,
                                          vty_info.cursor_y,
                                          prev_cursor_x,
                                          prev_cursor_y,
                                          has_prev);
                if (changed && present_handle != kInvalidDescriptor) {
                    uint8_t msg =
                        static_cast<uint8_t>(wm::ClientMessage::Present);
                    write_pipe_all(present_handle, &msg, sizeof(msg));
                }
            }
        }

        yield();
    }

    return 0;
}
