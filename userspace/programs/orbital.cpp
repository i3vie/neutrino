#include <stddef.h>
#include <stdint.h>

#include "descriptors.hpp"
#include "font8x8_basic.hpp"
#include "keyboard_scancode.hpp"
#include "wm_protocol.hpp"
#include "lattice/lattice.hpp"
#include "../crt/syscall.hpp"

namespace {

constexpr const char kRegistryName[] = "wm.registry";
constexpr const char kTitle[] = "Orbital";
constexpr uint32_t kFontWidth = 8;
constexpr uint32_t kFontHeight = 8;
constexpr uint32_t kPaddingX = 12;
constexpr uint32_t kPaddingY = 12;
constexpr uint32_t kCursorWidth = 2;
constexpr size_t kMaxText = 4096;
constexpr uint32_t kWindowWidth = 640;
constexpr uint32_t kWindowHeight = 400;
constexpr uint32_t kMenuIdSave = 1;
constexpr uint32_t kMenuIdLoad = 2;

struct Surface {
    uint8_t* buffer;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t bytes_per_pixel;
    wm::PixelFormat format;
};

struct Editor {
    char text[kMaxText];
    size_t length;
    size_t cursor;
};

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

void send_menu_update(uint32_t handle, const wm::MenuBar& bar) {
    if (handle == kInvalidDescriptor) {
        return;
    }
    wm::ClientMenuUpdate msg{};
    msg.type = static_cast<uint8_t>(wm::ClientMessage::MenuUpdate);
    msg.bar = bar;
    write_pipe_all(handle, &msg, sizeof(msg));
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

void fill_rect(const Surface& surface,
               int32_t x,
               int32_t y,
               uint32_t width,
               uint32_t height,
               uint32_t color) {
    lattice::fill_rect_stride(surface.buffer,
                              surface.width,
                              surface.height,
                              surface.stride,
                              surface.bytes_per_pixel,
                              x,
                              y,
                              width,
                              height,
                              color);
}

void draw_char(const Surface& surface,
               int32_t x,
               int32_t y,
               char ch,
               uint32_t color) {
    uint8_t uc = static_cast<uint8_t>(ch);
    if (uc >= 128) {
        uc = static_cast<uint8_t>('?');
    }
    for (uint32_t row = 0; row < kFontHeight; ++row) {
        uint8_t bits = font8x8_basic[uc][row];
        int32_t py = y + static_cast<int32_t>(row);
        if (py < 0 || py >= static_cast<int32_t>(surface.height)) {
            continue;
        }
        for (uint32_t col = 0; col < kFontWidth; ++col) {
            if ((bits & (1u << col)) == 0) {
                continue;
            }
            int32_t px = x + static_cast<int32_t>(col);
            if (px < 0 || px >= static_cast<int32_t>(surface.width)) {
                continue;
            }
            lattice::write_pixel(surface.buffer,
                                 surface.stride,
                                 surface.bytes_per_pixel,
                                 static_cast<uint32_t>(px),
                                 static_cast<uint32_t>(py),
                                 color);
        }
    }
}

void render_editor(const Surface& surface,
                   const Editor& editor,
                   uint32_t fg,
                   uint32_t bg,
                   uint32_t cursor) {
    fill_rect(surface, 0, 0, surface.width, surface.height, bg);

    if (surface.width <= kPaddingX * 2 ||
        surface.height <= kPaddingY * 2) {
        return;
    }

    uint32_t text_width = surface.width - kPaddingX * 2;
    uint32_t text_height = surface.height - kPaddingY * 2;
    uint32_t cols = text_width / kFontWidth;
    uint32_t rows = text_height / kFontHeight;
    if (cols == 0 || rows == 0) {
        return;
    }

    size_t cursor_index = editor.cursor;
    if (cursor_index > editor.length) {
        cursor_index = editor.length;
    }

    uint32_t col = 0;
    uint32_t row = 0;
    uint32_t cursor_col = 0;
    uint32_t cursor_row = 0;
    bool cursor_set = false;
    for (size_t i = 0; i < editor.length; ++i) {
        if (i == cursor_index && !cursor_set) {
            cursor_col = col;
            cursor_row = row;
            cursor_set = true;
        }
        char ch = editor.text[i];
        if (ch == '\r') {
            continue;
        }
        if (ch == '\n') {
            col = 0;
            ++row;
            if (row >= rows) {
                break;
            }
            continue;
        }
        if (col >= cols) {
            col = 0;
            ++row;
            if (row >= rows) {
                break;
            }
        }
        int32_t x = static_cast<int32_t>(kPaddingX + col * kFontWidth);
        int32_t y = static_cast<int32_t>(kPaddingY + row * kFontHeight);
        draw_char(surface, x, y, ch, fg);
        ++col;
    }

    if (!cursor_set) {
        cursor_col = col;
        cursor_row = row;
        cursor_set = true;
    }
    if (cursor_set && cursor_row < rows && cursor_col < cols) {
        int32_t cursor_x =
            static_cast<int32_t>(kPaddingX + cursor_col * kFontWidth);
        int32_t cursor_y =
            static_cast<int32_t>(kPaddingY + cursor_row * kFontHeight);
        fill_rect(surface,
                  cursor_x,
                  cursor_y,
                  kCursorWidth,
                  kFontHeight,
                  cursor);
    }
}

size_t line_start(const Editor& editor, size_t index) {
    if (index > editor.length) {
        index = editor.length;
    }
    while (index > 0 && editor.text[index - 1] != '\n') {
        --index;
    }
    return index;
}

size_t line_end(const Editor& editor, size_t index) {
    if (index > editor.length) {
        index = editor.length;
    }
    while (index < editor.length && editor.text[index] != '\n') {
        ++index;
    }
    return index;
}

bool move_cursor_left(Editor& editor) {
    if (editor.cursor == 0) {
        return false;
    }
    --editor.cursor;
    return true;
}

bool move_cursor_right(Editor& editor) {
    if (editor.cursor >= editor.length) {
        return false;
    }
    ++editor.cursor;
    return true;
}

bool move_cursor_up(Editor& editor) {
    size_t start = line_start(editor, editor.cursor);
    if (start == 0) {
        return false;
    }
    size_t prev_end = start - 1;
    size_t prev_start = line_start(editor, prev_end);
    size_t col = editor.cursor - start;
    size_t prev_len = prev_end - prev_start;
    if (col > prev_len) {
        col = prev_len;
    }
    editor.cursor = prev_start + col;
    return true;
}

bool move_cursor_down(Editor& editor) {
    size_t start = line_start(editor, editor.cursor);
    size_t end = line_end(editor, editor.cursor);
    if (end >= editor.length) {
        return false;
    }
    size_t next_start = end + 1;
    size_t next_end = line_end(editor, next_start);
    size_t col = editor.cursor - start;
    size_t next_len = next_end - next_start;
    if (col > next_len) {
        col = next_len;
    }
    editor.cursor = next_start + col;
    return true;
}

bool insert_char(Editor& editor, char ch) {
    if (editor.length + 1 >= kMaxText) {
        return false;
    }
    if (editor.cursor > editor.length) {
        editor.cursor = editor.length;
    }
    for (size_t i = editor.length; i > editor.cursor; --i) {
        editor.text[i] = editor.text[i - 1];
    }
    editor.text[editor.cursor] = ch;
    ++editor.length;
    ++editor.cursor;
    editor.text[editor.length] = '\0';
    return true;
}

bool backspace(Editor& editor) {
    if (editor.cursor == 0 || editor.length == 0) {
        return false;
    }
    size_t index = editor.cursor - 1;
    for (size_t i = index; i + 1 < editor.length; ++i) {
        editor.text[i] = editor.text[i + 1];
    }
    --editor.length;
    --editor.cursor;
    editor.text[editor.length] = '\0';
    return true;
}

bool handle_key_event(Editor& editor, const descriptor_defs::KeyboardEvent& event) {
    if (!keyboard::is_pressed(event)) {
        return false;
    }

    int32_t dx = 0;
    int32_t dy = 0;
    if (keyboard::is_arrow_key(event, dx, dy)) {
        if (dx < 0) {
            return move_cursor_left(editor);
        }
        if (dx > 0) {
            return move_cursor_right(editor);
        }
        if (dy < 0) {
            return move_cursor_up(editor);
        }
        if (dy > 0) {
            return move_cursor_down(editor);
        }
        return false;
    }

    if (keyboard::is_extended(event)) {
        return false;
    }

    char key = keyboard::scancode_to_char(event.scancode, event.mods);
    if (key == '\r') {
        key = '\n';
    }
    if (key == '\t') {
        key = ' ';
    }
    if (key == '\b' || key == 127) {
        return backspace(editor);
    }
    if (key < 32 && key != '\n') {
        return false;
    }
    return insert_char(editor, key);
}

void init_menu_bar(wm::MenuBar& bar) {
    bar = {};
    bar.menu_count = 1;
    copy_string(bar.menus[0].label, sizeof(bar.menus[0].label), "File");
    bar.menus[0].item_count = 2;
    copy_string(bar.menus[0].items[0].label,
                sizeof(bar.menus[0].items[0].label),
                "Save");
    bar.menus[0].items[0].id = kMenuIdSave;
    copy_string(bar.menus[0].items[1].label,
                sizeof(bar.menus[0].items[1].label),
                "Load");
    bar.menus[0].items[1].id = kMenuIdLoad;
}

bool save_to_file(const Editor& editor,
                  const lattice::FilePickerParent& parent) {
    lattice::FilePickerResult result =
        lattice::FilePicker::open(parent, lattice::FilePickerMode::Save);
    if (!result.accepted || result.handle == kInvalidDescriptor) {
        return true;
    }
    if (editor.length > 0) {
        file_write(result.handle, editor.text, editor.length);
    }
    file_close(result.handle);
    return true;
}

bool load_from_file(Editor& editor,
                    const lattice::FilePickerParent& parent) {
    lattice::FilePickerResult result =
        lattice::FilePicker::open(parent, lattice::FilePickerMode::Open);
    if (!result.accepted || result.handle == kInvalidDescriptor) {
        return true;
    }
    size_t total = 0;
    while (total + 1 < kMaxText) {
        long read = file_read(result.handle,
                              editor.text + total,
                              kMaxText - 1 - total);
        if (read <= 0) {
            break;
        }
        total += static_cast<size_t>(read);
    }
    file_close(result.handle);
    editor.length = total;
    editor.cursor = total;
    editor.text[total] = '\0';
    return true;
}

bool handle_menu_command(Editor& editor,
                         uint32_t id,
                         const lattice::FilePickerParent& parent) {
    switch (id) {
        case kMenuIdSave:
            return save_to_file(editor, parent);
        case kMenuIdLoad:
            return load_from_file(editor, parent);
        default:
            return false;
    }
}

}  // namespace

int main(uint64_t, uint64_t) {
    long registry_handle = shared_memory_open(kRegistryName, sizeof(wm::Registry));
    if (registry_handle < 0) {
        return 1;
    }
    descriptor_defs::SharedMemoryInfo registry_info{};
    if (shared_memory_get_info(static_cast<uint32_t>(registry_handle),
                               &registry_info) != 0 ||
        registry_info.base == 0 ||
        registry_info.length < sizeof(wm::Registry)) {
        return 1;
    }
    wm::Registry* registry =
        reinterpret_cast<wm::Registry*>(registry_info.base);
    while (registry->magic != wm::kRegistryMagic ||
           registry->version != wm::kRegistryVersion ||
           registry->server_pipe_id == 0) {
        yield();
    }

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
        pipe_open_existing(server_flags, registry->server_pipe_id);
    if (server_handle < 0) {
        descriptor_close(static_cast<uint32_t>(reply_handle));
        return 1;
    }

    wm::CreateRequest request{};
    request.type = static_cast<uint32_t>(wm::MessageType::CreateWindow);
    request.reply_pipe_id = reply_info.id;
    request.width = kWindowWidth;
    request.height = kWindowHeight;
    request.flags = 0;
    copy_string(request.title, sizeof(request.title), kTitle);

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
        descriptor_close(static_cast<uint32_t>(reply_handle));
        descriptor_close(static_cast<uint32_t>(shm_handle));
        return 1;
    }

    uint32_t bytes_per_pixel = (response.format.bpp + 7u) / 8u;
    if (bytes_per_pixel == 0 || bytes_per_pixel > 4) {
        descriptor_close(static_cast<uint32_t>(reply_handle));
        descriptor_close(static_cast<uint32_t>(shm_handle));
        return 1;
    }

    Surface surface{};
    surface.buffer = reinterpret_cast<uint8_t*>(shm_info.base);
    surface.width = response.width;
    surface.height = response.height;
    surface.stride = response.stride;
    if (surface.stride == 0) {
        surface.stride = surface.width * bytes_per_pixel;
    }
    surface.bytes_per_pixel = bytes_per_pixel;
    surface.format = response.format;

    lattice::FilePickerParent picker_parent{};
    picker_parent.buffer = surface.buffer;
    picker_parent.width = surface.width;
    picker_parent.height = surface.height;
    picker_parent.stride = surface.stride;
    picker_parent.bytes_per_pixel = surface.bytes_per_pixel;
    picker_parent.format = surface.format;
    picker_parent.reply_handle = static_cast<uint32_t>(reply_handle);
    picker_parent.present_handle = present_handle;

    uint32_t bg = lattice::pack_color(surface.format, 225, 230, 240);
    uint32_t fg = lattice::pack_color(surface.format, 18, 20, 26);
    uint32_t cursor = lattice::pack_color(surface.format, 120, 200, 160);

    Editor editor{};
    editor.text[0] = '\0';
    editor.length = 0;
    editor.cursor = 0;

    wm::MenuBar menu_bar{};
    init_menu_bar(menu_bar);

    render_editor(surface, editor, fg, bg, cursor);
    if (present_handle != kInvalidDescriptor) {
        uint8_t msg = static_cast<uint8_t>(wm::ClientMessage::Present);
        write_pipe_all(present_handle, &msg, sizeof(msg));
    }
    send_menu_update(present_handle, menu_bar);

    uint8_t buffer[128];
    size_t pending = 0;
    while (1) {
        long read = descriptor_read(static_cast<uint32_t>(reply_handle),
                                    buffer + pending,
                                    sizeof(buffer) - pending);
        if (read > 0) {
            pending += static_cast<size_t>(read);
        }

        bool changed = false;
        size_t offset = 0;
        while (offset < pending) {
            uint8_t type = buffer[offset];
            if (type == static_cast<uint8_t>(wm::ServerMessage::Close)) {
                return 0;
            }
            if (type == static_cast<uint8_t>(wm::ServerMessage::Mouse)) {
                if (pending - offset < sizeof(wm::ServerMouseMessage)) {
                    break;
                }
                offset += sizeof(wm::ServerMouseMessage);
                continue;
            }
            if (type == static_cast<uint8_t>(wm::ServerMessage::MenuCommand)) {
                if (pending - offset < sizeof(wm::ServerMenuCommand)) {
                    break;
                }
                wm::ServerMenuCommand msg{};
                lattice::copy_bytes(reinterpret_cast<uint8_t*>(&msg),
                                    buffer + offset,
                                    sizeof(msg));
                if (handle_menu_command(editor, msg.id, picker_parent)) {
                    changed = true;
                }
                offset += sizeof(msg);
                continue;
            }
            if (type == static_cast<uint8_t>(wm::ServerMessage::Key)) {
                if (pending - offset < sizeof(wm::ServerKeyMessage)) {
                    break;
                }
                wm::ServerKeyMessage msg{};
                lattice::copy_bytes(reinterpret_cast<uint8_t*>(&msg),
                                    buffer + offset,
                                    sizeof(msg));
                descriptor_defs::KeyboardEvent event{};
                event.scancode = msg.scancode;
                event.flags = msg.flags;
                event.mods = msg.mods;
                event.reserved = 0;
                if (handle_key_event(editor, event)) {
                    changed = true;
                }
                offset += sizeof(msg);
                continue;
            }
            offset += 1;
        }

        if (offset > 0 && offset < pending) {
            size_t remaining = pending - offset;
            for (size_t i = 0; i < remaining; ++i) {
                buffer[i] = buffer[offset + i];
            }
            pending = remaining;
        } else if (offset >= pending) {
            pending = 0;
        }

        if (changed) {
            render_editor(surface, editor, fg, bg, cursor);
            if (present_handle != kInvalidDescriptor) {
                uint8_t msg = static_cast<uint8_t>(wm::ClientMessage::Present);
                write_pipe_all(present_handle, &msg, sizeof(msg));
            }
        }

        yield();
    }
}
