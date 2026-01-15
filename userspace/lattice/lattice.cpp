#include "lattice.hpp"

#include "font8x8_basic.hpp"
#include "keyboard_scancode.hpp"
#include "../crt/syscall.hpp"

namespace lattice {

uint32_t scale_channel(uint32_t value, uint8_t mask_size) {
    if (mask_size == 0) {
        return 0;
    }
    if (mask_size >= 8) {
        return value;
    }
    uint32_t max_value = (1u << mask_size) - 1u;
    return (value * max_value + 127u) / 255u;
}

uint32_t pack_color(const wm::PixelFormat& fmt,
                    uint32_t r,
                    uint32_t g,
                    uint32_t b) {
    uint32_t rs = scale_channel(r, fmt.red_mask_size);
    uint32_t gs = scale_channel(g, fmt.green_mask_size);
    uint32_t bs = scale_channel(b, fmt.blue_mask_size);
    return (rs << fmt.red_mask_shift) |
           (gs << fmt.green_mask_shift) |
           (bs << fmt.blue_mask_shift);
}

uint32_t pack_color(const descriptor_defs::FramebufferInfo& info,
                    uint32_t r,
                    uint32_t g,
                    uint32_t b) {
    uint32_t rs = scale_channel(r, info.red_mask_size);
    uint32_t gs = scale_channel(g, info.green_mask_size);
    uint32_t bs = scale_channel(b, info.blue_mask_size);
    return (rs << info.red_mask_shift) |
           (gs << info.green_mask_shift) |
           (bs << info.blue_mask_shift);
}

void store_pixel(uint8_t* dest,
                 uint32_t bytes_per_pixel,
                 uint32_t pixel) {
    if (dest == nullptr) {
        return;
    }
    for (uint32_t byte = 0; byte < bytes_per_pixel; ++byte) {
        dest[byte] = static_cast<uint8_t>((pixel >> (8u * byte)) & 0xFFu);
    }
}

void write_pixel(uint8_t* buffer,
                 uint32_t stride,
                 uint32_t bytes_per_pixel,
                 uint32_t x,
                 uint32_t y,
                 uint32_t pixel) {
    if (buffer == nullptr) {
        return;
    }
    size_t offset = static_cast<size_t>(y) * stride +
                    static_cast<size_t>(x) * bytes_per_pixel;
    store_pixel(buffer + offset, bytes_per_pixel, pixel);
}

void write_pixel(uint8_t* buffer,
                 const descriptor_defs::FramebufferInfo& info,
                 uint32_t bytes_per_pixel,
                 uint32_t x,
                 uint32_t y,
                 uint32_t pixel) {
    if (buffer == nullptr) {
        return;
    }
    size_t offset = static_cast<size_t>(y) * info.pitch +
                    static_cast<size_t>(x) * bytes_per_pixel;
    store_pixel(buffer + offset, bytes_per_pixel, pixel);
}

void write_pixel_raw(uint8_t* buffer,
                     uint32_t bytes_per_pixel,
                     size_t offset,
                     uint32_t pixel) {
    if (buffer == nullptr) {
        return;
    }
    for (uint32_t byte = 0; byte < bytes_per_pixel; ++byte) {
        buffer[offset + byte] =
            static_cast<uint8_t>((pixel >> (8u * byte)) & 0xFFu);
    }
}

void copy_bytes(uint8_t* dest, const uint8_t* src, size_t count) {
    if (dest == nullptr || src == nullptr || count == 0) {
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        dest[i] = src[i];
    }
}

void fill_rect(uint8_t* frame,
               const descriptor_defs::FramebufferInfo& info,
               uint32_t bytes_per_pixel,
               int32_t x,
               int32_t y,
               uint32_t width,
               uint32_t height,
               uint32_t color) {
    if (frame == nullptr || width == 0 || height == 0) {
        return;
    }
    int32_t left = x;
    int32_t top = y;
    int32_t right = x + static_cast<int32_t>(width);
    int32_t bottom = y + static_cast<int32_t>(height);
    if (right <= 0 || bottom <= 0) {
        return;
    }
    if (left >= static_cast<int32_t>(info.width) ||
        top >= static_cast<int32_t>(info.height)) {
        return;
    }
    if (left < 0) {
        left = 0;
    }
    if (top < 0) {
        top = 0;
    }
    if (right > static_cast<int32_t>(info.width)) {
        right = static_cast<int32_t>(info.width);
    }
    if (bottom > static_cast<int32_t>(info.height)) {
        bottom = static_cast<int32_t>(info.height);
    }
    for (int32_t py = top; py < bottom; ++py) {
        for (int32_t px = left; px < right; ++px) {
            write_pixel(frame,
                        info,
                        bytes_per_pixel,
                        static_cast<uint32_t>(px),
                        static_cast<uint32_t>(py),
                        color);
        }
    }
}

void fill_rect_stride(uint8_t* buffer,
                      uint32_t width,
                      uint32_t height,
                      uint32_t stride,
                      uint32_t bytes_per_pixel,
                      int32_t x,
                      int32_t y,
                      uint32_t rect_width,
                      uint32_t rect_height,
                      uint32_t color) {
    if (buffer == nullptr || rect_width == 0 || rect_height == 0) {
        return;
    }
    int32_t left = x;
    int32_t top = y;
    int32_t right = x + static_cast<int32_t>(rect_width);
    int32_t bottom = y + static_cast<int32_t>(rect_height);
    if (right <= 0 || bottom <= 0) {
        return;
    }
    if (left < 0) {
        left = 0;
    }
    if (top < 0) {
        top = 0;
    }
    if (right > static_cast<int32_t>(width)) {
        right = static_cast<int32_t>(width);
    }
    if (bottom > static_cast<int32_t>(height)) {
        bottom = static_cast<int32_t>(height);
    }
    for (int32_t py = top; py < bottom; ++py) {
        for (int32_t px = left; px < right; ++px) {
            write_pixel(buffer,
                        stride,
                        bytes_per_pixel,
                        static_cast<uint32_t>(px),
                        static_cast<uint32_t>(py),
                        color);
        }
    }
}

namespace {

constexpr uint32_t kFontWidth = 8;
constexpr uint32_t kFontHeight = 8;
constexpr uint32_t kPickerPadding = 10;
constexpr uint32_t kPickerHeaderHeight = 26;
constexpr uint32_t kPickerFooterHeight = 36;
constexpr uint32_t kPickerRowHeight = 12;
constexpr uint32_t kPickerButtonWidth = 64;
constexpr uint32_t kPickerButtonHeight = 16;
constexpr uint32_t kPickerInputHeight = 16;
constexpr uint32_t kPickerMaxEntries = 64;
constexpr uint32_t kPickerMaxDepth = 8;
constexpr uint32_t kPickerNameMax = 64;
constexpr uint32_t kInvalidFileHandle = 0xFFFFFFFFu;

size_t string_length(const char* text) {
    if (text == nullptr) {
        return 0;
    }
    size_t len = 0;
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

bool strings_equal(const char* a, const char* b) {
    if (a == nullptr || b == nullptr) {
        return false;
    }
    size_t i = 0;
    while (a[i] != '\0' && b[i] != '\0') {
        if (a[i] != b[i]) {
            return false;
        }
        ++i;
    }
    return a[i] == '\0' && b[i] == '\0';
}

void append_text(char* dest, size_t dest_size, const char* src) {
    if (dest == nullptr || dest_size == 0) {
        return;
    }
    size_t len = string_length(dest);
    if (len + 1 >= dest_size) {
        return;
    }
    size_t idx = 0;
    while (src != nullptr && src[idx] != '\0' && len + 1 < dest_size) {
        dest[len++] = src[idx++];
    }
    dest[len] = '\0';
}

void draw_char(uint8_t* buffer,
               uint32_t stride,
               uint32_t bytes_per_pixel,
               int32_t x,
               int32_t y,
               char ch,
               uint32_t color,
               uint32_t width,
               uint32_t height) {
    uint8_t uc = static_cast<uint8_t>(ch);
    if (uc >= 128) {
        uc = static_cast<uint8_t>('?');
    }
    for (uint32_t row = 0; row < kFontHeight; ++row) {
        uint8_t bits = font8x8_basic[uc][row];
        int32_t py = y + static_cast<int32_t>(row);
        if (py < 0 || py >= static_cast<int32_t>(height)) {
            continue;
        }
        for (uint32_t col = 0; col < kFontWidth; ++col) {
            if ((bits & (1u << col)) == 0) {
                continue;
            }
            int32_t px = x + static_cast<int32_t>(col);
            if (px < 0 || px >= static_cast<int32_t>(width)) {
                continue;
            }
            write_pixel(buffer,
                        stride,
                        bytes_per_pixel,
                        static_cast<uint32_t>(px),
                        static_cast<uint32_t>(py),
                        color);
        }
    }
}

void draw_text(uint8_t* buffer,
               uint32_t stride,
               uint32_t bytes_per_pixel,
               int32_t x,
               int32_t y,
               const char* text,
               uint32_t color,
               uint32_t width,
               uint32_t height) {
    if (text == nullptr) {
        return;
    }
    int32_t cursor = x;
    for (size_t i = 0; text[i] != '\0'; ++i) {
        draw_char(buffer,
                  stride,
                  bytes_per_pixel,
                  cursor,
                  y,
                  text[i],
                  color,
                  width,
                  height);
        cursor += static_cast<int32_t>(kFontWidth);
    }
}

bool point_in_rect(uint16_t px,
                   uint16_t py,
                   uint32_t x,
                   uint32_t y,
                   uint32_t width,
                   uint32_t height) {
    return px >= x && py >= y &&
           px < x + width && py < y + height;
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

void send_present(uint32_t handle) {
    if (handle == kInvalidFileHandle) {
        return;
    }
    uint8_t msg = static_cast<uint8_t>(wm::ClientMessage::Present);
    write_pipe_all(handle, &msg, sizeof(msg));
}

struct PickerState {
    uint32_t dir_handle;
    uint8_t path_depth;
    char path_segments[kPickerMaxDepth][kPickerNameMax];
    DirEntry* entries;
    size_t entry_count;
    int selected;
    uint32_t scroll;
    char filename[kPickerNameMax];
    bool confirm_overwrite;
};

bool read_entries(PickerState& state) {
    state.entry_count = 0;
    state.selected = -1;
    state.scroll = 0;
    DirEntry entry{};
    while (state.entry_count < kPickerMaxEntries) {
        long read = directory_read(state.dir_handle, &entry);
        if (read <= 0) {
            break;
        }
        if (entry.name[0] == '\0') {
            continue;
        }
        if (strings_equal(entry.name, ".") || strings_equal(entry.name, "..")) {
            continue;
        }
        state.entries[state.entry_count++] = entry;
    }
    if (state.entry_count > 0) {
        state.selected = 0;
    }
    return true;
}

bool open_current_directory(PickerState& state) {
    if (state.dir_handle != kInvalidFileHandle) {
        directory_close(state.dir_handle);
    }
    long handle = directory_open_root();
    if (handle < 0) {
        state.dir_handle = kInvalidFileHandle;
        state.entry_count = 0;
        state.selected = -1;
        return false;
    }
    for (uint8_t i = 0; i < state.path_depth; ++i) {
        long child = directory_open_at(static_cast<uint32_t>(handle),
                                       state.path_segments[i]);
        directory_close(static_cast<uint32_t>(handle));
        if (child < 0) {
            state.dir_handle = kInvalidFileHandle;
            state.entry_count = 0;
            state.selected = -1;
            return false;
        }
        handle = child;
    }
    state.dir_handle = static_cast<uint32_t>(handle);
    return read_entries(state);
}

bool is_dir_entry(const DirEntry& entry) {
    return (entry.flags & DIR_ENTRY_FLAG_DIRECTORY) != 0;
}

bool update_scroll(PickerState& state, uint32_t visible_rows) {
    if (state.selected < 0) {
        state.scroll = 0;
        return false;
    }
    uint32_t sel = static_cast<uint32_t>(state.selected);
    if (visible_rows == 0) {
        state.scroll = 0;
        return false;
    }
    if (sel < state.scroll) {
        state.scroll = sel;
        return true;
    }
    if (sel >= state.scroll + visible_rows) {
        state.scroll = sel - visible_rows + 1;
        return true;
    }
    return false;
}

bool file_exists(uint32_t dir_handle, const char* name) {
    long handle = file_open_at(dir_handle, name);
    if (handle < 0) {
        return false;
    }
    file_close(static_cast<uint32_t>(handle));
    return true;
}

}  // namespace

FilePickerResult FilePicker::open(const FilePickerParent& parent,
                                  FilePickerMode mode) {
    FilePickerResult result{};
    result.accepted = false;
    result.handle = kInvalidFileHandle;

    size_t entries_bytes = sizeof(DirEntry) * kPickerMaxEntries;
    DirEntry* entries = reinterpret_cast<DirEntry*>(
        map_anonymous(entries_bytes, MAP_WRITE));
    if (entries == nullptr) {
        return result;
    }

    PickerState state{};
    state.entries = entries;
    state.dir_handle = kInvalidFileHandle;
    state.path_depth = 0;
    state.filename[0] = '\0';
    state.confirm_overwrite = false;

    if (!open_current_directory(state)) {
        unmap(entries, entries_bytes);
        return result;
    }

    uint32_t bg = pack_color(parent.format, 18, 20, 26);
    uint32_t panel = pack_color(parent.format, 26, 30, 38);
    uint32_t panel_border = pack_color(parent.format, 10, 12, 18);
    uint32_t text = pack_color(parent.format, 230, 235, 245);
    uint32_t muted = pack_color(parent.format, 150, 160, 175);
    uint32_t highlight = pack_color(parent.format, 60, 90, 160);
    uint32_t button = pack_color(parent.format, 60, 110, 180);
    uint32_t button_disabled = pack_color(parent.format, 40, 50, 70);
    uint32_t overlay = pack_color(parent.format, 10, 12, 18);

    auto visible_rows_for_parent = [&]() -> uint32_t {
        uint32_t chrome = kPickerPadding * 2 + kPickerHeaderHeight + kPickerFooterHeight;
        if (parent.height <= chrome || kPickerRowHeight == 0) {
            return 0;
        }
        uint32_t list_h = parent.height - chrome;
        return list_h / kPickerRowHeight;
    };

    auto render = [&]() {
        fill_rect_stride(parent.buffer,
                         parent.width,
                         parent.height,
                         parent.stride,
                         parent.bytes_per_pixel,
                         0,
                         0,
                         parent.width,
                         parent.height,
                         bg);

        int32_t panel_x = static_cast<int32_t>(kPickerPadding);
        int32_t panel_y = static_cast<int32_t>(kPickerPadding);
        uint32_t panel_w =
            (parent.width > kPickerPadding * 2)
                ? parent.width - kPickerPadding * 2
                : parent.width;
        uint32_t panel_h =
            (parent.height > kPickerPadding * 2)
                ? parent.height - kPickerPadding * 2
                : parent.height;

        fill_rect_stride(parent.buffer,
                         parent.width,
                         parent.height,
                         parent.stride,
                         parent.bytes_per_pixel,
                         panel_x,
                         panel_y,
                         panel_w,
                         panel_h,
                         panel);
        fill_rect_stride(parent.buffer,
                         parent.width,
                         parent.height,
                         parent.stride,
                         parent.bytes_per_pixel,
                         panel_x,
                         panel_y,
                         panel_w,
                         1,
                         panel_border);
        fill_rect_stride(parent.buffer,
                         parent.width,
                         parent.height,
                         parent.stride,
                         parent.bytes_per_pixel,
                         panel_x,
                         panel_y + static_cast<int32_t>(panel_h - 1),
                         panel_w,
                         1,
                         panel_border);
        fill_rect_stride(parent.buffer,
                         parent.width,
                         parent.height,
                         parent.stride,
                         parent.bytes_per_pixel,
                         panel_x,
                         panel_y,
                         1,
                         panel_h,
                         panel_border);
        fill_rect_stride(parent.buffer,
                         parent.width,
                         parent.height,
                         parent.stride,
                         parent.bytes_per_pixel,
                         panel_x + static_cast<int32_t>(panel_w - 1),
                         panel_y,
                         1,
                         panel_h,
                         panel_border);

        const char* title =
            (mode == FilePickerMode::Open) ? "Open File" : "Save File";
        draw_text(parent.buffer,
                  parent.stride,
                  parent.bytes_per_pixel,
                  panel_x + 8,
                  panel_y + 6,
                  title,
                  text,
                  parent.width,
                  parent.height);

        char location[128];
        copy_string(location, sizeof(location), "/");
        for (uint8_t i = 0; i < state.path_depth; ++i) {
            if (location[1] != '\0') {
                append_text(location, sizeof(location), "/");
            }
            append_text(location, sizeof(location), state.path_segments[i]);
        }
        draw_text(parent.buffer,
                  parent.stride,
                  parent.bytes_per_pixel,
                  panel_x + 8,
                  panel_y + 18,
                  location,
                  muted,
                  parent.width,
                  parent.height);

        uint32_t up_w = kPickerButtonWidth - 8;
        uint32_t up_h = kPickerButtonHeight;
        uint32_t up_x =
            panel_x + panel_w - up_w - 8;
        uint32_t up_y = panel_y + 6;
        fill_rect_stride(parent.buffer,
                         parent.width,
                         parent.height,
                         parent.stride,
                         parent.bytes_per_pixel,
                         static_cast<int32_t>(up_x),
                         static_cast<int32_t>(up_y),
                         up_w,
                         up_h,
                         button);
        draw_text(parent.buffer,
                  parent.stride,
                  parent.bytes_per_pixel,
                  static_cast<int32_t>(up_x + 8),
                  static_cast<int32_t>(up_y + 4),
                  "Up",
                  text,
                  parent.width,
                  parent.height);

        uint32_t list_x = panel_x + 8;
        uint32_t list_y = panel_y + kPickerHeaderHeight;
        uint32_t list_w = panel_w - 16;
        uint32_t list_h = panel_h - kPickerHeaderHeight - kPickerFooterHeight;
        uint32_t visible_rows =
            (kPickerRowHeight > 0) ? (list_h / kPickerRowHeight) : 0;

        for (uint32_t row = 0; row < visible_rows; ++row) {
            uint32_t idx = state.scroll + row;
            if (idx >= state.entry_count) {
                break;
            }
            const auto& entry = state.entries[idx];
            bool selected = static_cast<int>(idx) == state.selected;
            if (selected) {
                fill_rect_stride(parent.buffer,
                                 parent.width,
                                 parent.height,
                                 parent.stride,
                                 parent.bytes_per_pixel,
                                 static_cast<int32_t>(list_x),
                                 static_cast<int32_t>(list_y +
                                                      row * kPickerRowHeight),
                                 list_w,
                                 kPickerRowHeight,
                                 highlight);
            }
            char label[68];
            copy_string(label, sizeof(label), entry.name);
            if (is_dir_entry(entry)) {
                append_text(label, sizeof(label), "/");
            }
            draw_text(parent.buffer,
                      parent.stride,
                      parent.bytes_per_pixel,
                      static_cast<int32_t>(list_x + 4),
                      static_cast<int32_t>(list_y +
                                           row * kPickerRowHeight + 2),
                      label,
                      text,
                      parent.width,
                      parent.height);
        }

        uint32_t footer_y = panel_y + panel_h - kPickerFooterHeight;
        uint32_t cancel_w = kPickerButtonWidth;
        uint32_t action_w = kPickerButtonWidth;
        uint32_t action_x =
            panel_x + panel_w - action_w - 8;
        uint32_t cancel_x =
            action_x - cancel_w - 8;
        uint32_t button_y =
            footer_y + (kPickerFooterHeight - kPickerButtonHeight) / 2;

        bool can_accept = false;
        if (mode == FilePickerMode::Open) {
            if (state.selected >= 0 &&
                static_cast<size_t>(state.selected) < state.entry_count) {
                can_accept = !is_dir_entry(state.entries[state.selected]);
            }
        } else {
            can_accept = state.filename[0] != '\0';
        }

        fill_rect_stride(parent.buffer,
                         parent.width,
                         parent.height,
                         parent.stride,
                         parent.bytes_per_pixel,
                         static_cast<int32_t>(cancel_x),
                         static_cast<int32_t>(button_y),
                         cancel_w,
                         kPickerButtonHeight,
                         button);
        draw_text(parent.buffer,
                  parent.stride,
                  parent.bytes_per_pixel,
                  static_cast<int32_t>(cancel_x + 8),
                  static_cast<int32_t>(button_y + 4),
                  "Cancel",
                  text,
                  parent.width,
                  parent.height);

        uint32_t action_color = can_accept ? button : button_disabled;
        fill_rect_stride(parent.buffer,
                         parent.width,
                         parent.height,
                         parent.stride,
                         parent.bytes_per_pixel,
                         static_cast<int32_t>(action_x),
                         static_cast<int32_t>(button_y),
                         action_w,
                         kPickerButtonHeight,
                         action_color);
        const char* action_label =
            (mode == FilePickerMode::Open) ? "Open" : "Save";
        draw_text(parent.buffer,
                  parent.stride,
                  parent.bytes_per_pixel,
                  static_cast<int32_t>(action_x + 10),
                  static_cast<int32_t>(button_y + 4),
                  action_label,
                  text,
                  parent.width,
                  parent.height);

        if (mode == FilePickerMode::Save) {
            uint32_t input_w = cancel_x - (panel_x + 8) - 8;
            uint32_t input_x = panel_x + 8;
            uint32_t input_y =
                footer_y + (kPickerFooterHeight - kPickerInputHeight) / 2;
            fill_rect_stride(parent.buffer,
                             parent.width,
                             parent.height,
                             parent.stride,
                             parent.bytes_per_pixel,
                             static_cast<int32_t>(input_x),
                             static_cast<int32_t>(input_y),
                             input_w,
                             kPickerInputHeight,
                             panel_border);
            draw_text(parent.buffer,
                      parent.stride,
                      parent.bytes_per_pixel,
                      static_cast<int32_t>(input_x + 4),
                      static_cast<int32_t>(input_y + 4),
                      state.filename,
                      text,
                      parent.width,
                      parent.height);
        }

        if (state.confirm_overwrite) {
            uint32_t overlay_w = panel_w - 40;
            uint32_t overlay_h = 80;
            int32_t overlay_x =
                panel_x + static_cast<int32_t>((panel_w - overlay_w) / 2);
            int32_t overlay_y =
                panel_y + static_cast<int32_t>((panel_h - overlay_h) / 2);
            fill_rect_stride(parent.buffer,
                             parent.width,
                             parent.height,
                             parent.stride,
                             parent.bytes_per_pixel,
                             overlay_x,
                             overlay_y,
                             overlay_w,
                             overlay_h,
                             overlay);
            fill_rect_stride(parent.buffer,
                             parent.width,
                             parent.height,
                             parent.stride,
                             parent.bytes_per_pixel,
                             overlay_x,
                             overlay_y,
                             overlay_w,
                             1,
                             panel_border);
            fill_rect_stride(parent.buffer,
                             parent.width,
                             parent.height,
                             parent.stride,
                             parent.bytes_per_pixel,
                             overlay_x,
                             overlay_y + static_cast<int32_t>(overlay_h - 1),
                             overlay_w,
                             1,
                             panel_border);
            fill_rect_stride(parent.buffer,
                             parent.width,
                             parent.height,
                             parent.stride,
                             parent.bytes_per_pixel,
                             overlay_x,
                             overlay_y,
                             1,
                             overlay_h,
                             panel_border);
            fill_rect_stride(parent.buffer,
                             parent.width,
                             parent.height,
                             parent.stride,
                             parent.bytes_per_pixel,
                             overlay_x + static_cast<int32_t>(overlay_w - 1),
                             overlay_y,
                             1,
                             overlay_h,
                             panel_border);
            draw_text(parent.buffer,
                      parent.stride,
                      parent.bytes_per_pixel,
                      overlay_x + 10,
                      overlay_y + 12,
                      "Overwrite existing file?",
                      text,
                      parent.width,
                      parent.height);
            uint32_t yes_x = static_cast<uint32_t>(overlay_x + 12);
            uint32_t no_x = static_cast<uint32_t>(overlay_x + overlay_w - 12 - kPickerButtonWidth);
            uint32_t yes_y = static_cast<uint32_t>(overlay_y + overlay_h - 28);
            fill_rect_stride(parent.buffer,
                             parent.width,
                             parent.height,
                             parent.stride,
                             parent.bytes_per_pixel,
                             static_cast<int32_t>(yes_x),
                             static_cast<int32_t>(yes_y),
                             kPickerButtonWidth,
                             kPickerButtonHeight,
                             button);
            fill_rect_stride(parent.buffer,
                             parent.width,
                             parent.height,
                             parent.stride,
                             parent.bytes_per_pixel,
                             static_cast<int32_t>(no_x),
                             static_cast<int32_t>(yes_y),
                             kPickerButtonWidth,
                             kPickerButtonHeight,
                             button);
            draw_text(parent.buffer,
                      parent.stride,
                      parent.bytes_per_pixel,
                      static_cast<int32_t>(yes_x + 10),
                      static_cast<int32_t>(yes_y + 4),
                      "Yes",
                      text,
                      parent.width,
                      parent.height);
            draw_text(parent.buffer,
                      parent.stride,
                      parent.bytes_per_pixel,
                      static_cast<int32_t>(no_x + 12),
                      static_cast<int32_t>(yes_y + 4),
                      "No",
                      text,
                      parent.width,
                      parent.height);
        }
    };

    bool needs_redraw = true;
    uint8_t buffer[256];
    size_t pending = 0;

    while (true) {
        if (needs_redraw) {
            render();
            send_present(parent.present_handle);
            needs_redraw = false;
        }

        long read = descriptor_read(parent.reply_handle,
                                    buffer + pending,
                                    sizeof(buffer) - pending);
        if (read > 0) {
            pending += static_cast<size_t>(read);
        }

        size_t offset = 0;
        while (offset < pending) {
            uint8_t type = buffer[offset];
            if (type == static_cast<uint8_t>(wm::ServerMessage::Close)) {
                directory_close(state.dir_handle);
                unmap(entries, entries_bytes);
                result.accepted = false;
                result.handle = kInvalidFileHandle;
                return result;
            }
            if (type == static_cast<uint8_t>(wm::ServerMessage::Mouse)) {
                if (pending - offset < sizeof(wm::ServerMouseMessage)) {
                    break;
                }
                wm::ServerMouseMessage msg{};
                copy_bytes(reinterpret_cast<uint8_t*>(&msg),
                           buffer + offset,
                           sizeof(msg));
                offset += sizeof(msg);
                if ((msg.buttons & 0x1u) != 0) {
                    int32_t panel_x = static_cast<int32_t>(kPickerPadding);
                    int32_t panel_y = static_cast<int32_t>(kPickerPadding);
                    uint32_t panel_w =
                        (parent.width > kPickerPadding * 2)
                            ? parent.width - kPickerPadding * 2
                            : parent.width;
                    uint32_t panel_h =
                        (parent.height > kPickerPadding * 2)
                            ? parent.height - kPickerPadding * 2
                            : parent.height;
                    uint32_t up_w = kPickerButtonWidth - 8;
                    uint32_t up_h = kPickerButtonHeight;
                    uint32_t up_x =
                        static_cast<uint32_t>(panel_x + panel_w - up_w - 8);
                    uint32_t up_y = static_cast<uint32_t>(panel_y + 6);

                    uint32_t list_x = static_cast<uint32_t>(panel_x + 8);
                    uint32_t list_y = static_cast<uint32_t>(panel_y + kPickerHeaderHeight);
                    uint32_t list_w = panel_w - 16;
                    uint32_t list_h = panel_h - kPickerHeaderHeight - kPickerFooterHeight;
                    uint32_t visible_rows =
                        (kPickerRowHeight > 0) ? (list_h / kPickerRowHeight) : 0;

                    uint32_t footer_y = static_cast<uint32_t>(panel_y + panel_h - kPickerFooterHeight);
                    uint32_t action_x =
                        static_cast<uint32_t>(panel_x + panel_w - kPickerButtonWidth - 8);
                    uint32_t cancel_x =
                        static_cast<uint32_t>(action_x - kPickerButtonWidth - 8);
                    uint32_t button_y =
                        footer_y + (kPickerFooterHeight - kPickerButtonHeight) / 2;

                    if (state.confirm_overwrite) {
                        uint32_t overlay_w = panel_w - 40;
                        uint32_t overlay_h = 80;
                        uint32_t overlay_x =
                            static_cast<uint32_t>(panel_x + (panel_w - overlay_w) / 2);
                        uint32_t overlay_y =
                            static_cast<uint32_t>(panel_y + (panel_h - overlay_h) / 2);
                        uint32_t yes_x = overlay_x + 12;
                        uint32_t no_x = overlay_x + overlay_w - 12 - kPickerButtonWidth;
                        uint32_t yes_y = overlay_y + overlay_h - 28;
                        if (point_in_rect(msg.x, msg.y,
                                          yes_x,
                                          yes_y,
                                          kPickerButtonWidth,
                                          kPickerButtonHeight)) {
                            long handle = file_create_at(state.dir_handle,
                                                         state.filename);
                            directory_close(state.dir_handle);
                            unmap(entries, entries_bytes);
                            result.accepted = handle >= 0;
                            result.handle = (handle >= 0)
                                ? static_cast<uint32_t>(handle)
                                : kInvalidFileHandle;
                            return result;
                        }
                        if (point_in_rect(msg.x, msg.y,
                                          no_x,
                                          yes_y,
                                          kPickerButtonWidth,
                                          kPickerButtonHeight)) {
                            state.confirm_overwrite = false;
                            needs_redraw = true;
                        }
                        continue;
                    }

                    if (point_in_rect(msg.x, msg.y, up_x, up_y, up_w, up_h)) {
                        if (state.path_depth > 0) {
                            --state.path_depth;
                            open_current_directory(state);
                            needs_redraw = true;
                        }
                        continue;
                    }

                    if (point_in_rect(msg.x, msg.y,
                                      list_x,
                                      list_y,
                                      list_w,
                                      list_h)) {
                        uint32_t rel_y = msg.y - list_y;
                        uint32_t row = (kPickerRowHeight > 0)
                            ? (rel_y / kPickerRowHeight)
                            : 0;
                        uint32_t idx = state.scroll + row;
                        if (idx < state.entry_count) {
                            const auto& entry = state.entries[idx];
                            if (is_dir_entry(entry)) {
                                if (state.path_depth < kPickerMaxDepth) {
                                    copy_string(state.path_segments[state.path_depth],
                                                sizeof(state.path_segments[state.path_depth]),
                                                entry.name);
                                    ++state.path_depth;
                                    open_current_directory(state);
                                    needs_redraw = true;
                                }
                            } else {
                                state.selected = static_cast<int>(idx);
                                update_scroll(state, visible_rows);
                                if (mode == FilePickerMode::Save) {
                                    copy_string(state.filename,
                                                sizeof(state.filename),
                                                entry.name);
                                }
                                needs_redraw = true;
                            }
                        }
                        continue;
                    }

                    if (point_in_rect(msg.x, msg.y,
                                      cancel_x,
                                      button_y,
                                      kPickerButtonWidth,
                                      kPickerButtonHeight)) {
                        directory_close(state.dir_handle);
                        unmap(entries, entries_bytes);
                        result.accepted = false;
                        result.handle = kInvalidFileHandle;
                        return result;
                    }

                    bool can_accept = false;
                    if (mode == FilePickerMode::Open) {
                        if (state.selected >= 0 &&
                            static_cast<size_t>(state.selected) < state.entry_count) {
                            can_accept = !is_dir_entry(state.entries[state.selected]);
                        }
                    } else {
                        can_accept = state.filename[0] != '\0';
                    }
                    if (can_accept &&
                        point_in_rect(msg.x, msg.y,
                                      action_x,
                                      button_y,
                                      kPickerButtonWidth,
                                      kPickerButtonHeight)) {
                        if (mode == FilePickerMode::Open) {
                            const auto& entry =
                                state.entries[static_cast<size_t>(state.selected)];
                            long handle = file_open_at(state.dir_handle,
                                                       entry.name);
                            directory_close(state.dir_handle);
                            unmap(entries, entries_bytes);
                            result.accepted = handle >= 0;
                            result.handle = (handle >= 0)
                                ? static_cast<uint32_t>(handle)
                                : kInvalidFileHandle;
                            return result;
                        } else {
                            if (file_exists(state.dir_handle, state.filename)) {
                                state.confirm_overwrite = true;
                                needs_redraw = true;
                            } else {
                                long handle = file_create_at(state.dir_handle,
                                                             state.filename);
                                directory_close(state.dir_handle);
                                unmap(entries, entries_bytes);
                                result.accepted = handle >= 0;
                                result.handle = (handle >= 0)
                                    ? static_cast<uint32_t>(handle)
                                    : kInvalidFileHandle;
                                return result;
                            }
                        }
                        continue;
                    }
                }
                continue;
            }
            if (type == static_cast<uint8_t>(wm::ServerMessage::Key)) {
                if (pending - offset < sizeof(wm::ServerKeyMessage)) {
                    break;
                }
                wm::ServerKeyMessage msg{};
                copy_bytes(reinterpret_cast<uint8_t*>(&msg),
                           buffer + offset,
                           sizeof(msg));
                offset += sizeof(msg);
                descriptor_defs::KeyboardEvent event{};
                event.scancode = msg.scancode;
                event.flags = msg.flags;
                event.mods = msg.mods;
                event.reserved = 0;
                if (!keyboard::is_pressed(event)) {
                    continue;
                }

                if (state.confirm_overwrite) {
                    if (!keyboard::is_extended(event) &&
                        event.scancode == 0x01) {
                        state.confirm_overwrite = false;
                        needs_redraw = true;
                    } else if (!keyboard::is_extended(event) &&
                               event.scancode == 0x1C) {
                        long handle = file_create_at(state.dir_handle,
                                                     state.filename);
                        directory_close(state.dir_handle);
                        unmap(entries, entries_bytes);
                        result.accepted = handle >= 0;
                        result.handle = (handle >= 0)
                            ? static_cast<uint32_t>(handle)
                            : kInvalidFileHandle;
                        return result;
                    }
                    continue;
                }

                int32_t dx = 0;
                int32_t dy = 0;
                if (keyboard::is_arrow_key(event, dx, dy)) {
                    if (dy < 0 && state.selected > 0) {
                        --state.selected;
                        update_scroll(state, visible_rows_for_parent());
                        needs_redraw = true;
                    } else if (dy > 0 &&
                               state.selected + 1 < static_cast<int>(state.entry_count)) {
                        ++state.selected;
                        update_scroll(state, visible_rows_for_parent());
                        needs_redraw = true;
                    }
                    continue;
                }

                if (!keyboard::is_extended(event) &&
                    event.scancode == 0x01) {
                    directory_close(state.dir_handle);
                    unmap(entries, entries_bytes);
                    result.accepted = false;
                    result.handle = kInvalidFileHandle;
                    return result;
                }

                if (mode == FilePickerMode::Save) {
                    char ch = keyboard::scancode_to_char(event.scancode,
                                                         event.mods);
                    if (ch == '\b') {
                        size_t len = string_length(state.filename);
                        if (len > 0) {
                            state.filename[len - 1] = '\0';
                            needs_redraw = true;
                        }
                    } else if (ch >= 32 && ch <= 126 && ch != '/') {
                        size_t len = string_length(state.filename);
                        if (len + 1 < sizeof(state.filename)) {
                            state.filename[len] = ch;
                            state.filename[len + 1] = '\0';
                            needs_redraw = true;
                        }
                    } else if (ch == '\n') {
                        if (state.filename[0] != '\0') {
                            if (file_exists(state.dir_handle, state.filename)) {
                                state.confirm_overwrite = true;
                                needs_redraw = true;
                            } else {
                                long handle = file_create_at(state.dir_handle,
                                                             state.filename);
                                directory_close(state.dir_handle);
                                unmap(entries, entries_bytes);
                                result.accepted = handle >= 0;
                                result.handle = (handle >= 0)
                                    ? static_cast<uint32_t>(handle)
                                    : kInvalidFileHandle;
                                return result;
                            }
                        }
                    }
                } else {
                    if (!keyboard::is_extended(event) &&
                        event.scancode == 0x1C &&
                        state.selected >= 0 &&
                        static_cast<size_t>(state.selected) < state.entry_count) {
                        const auto& entry =
                            state.entries[static_cast<size_t>(state.selected)];
                        if (is_dir_entry(entry)) {
                            if (state.path_depth < kPickerMaxDepth) {
                                copy_string(state.path_segments[state.path_depth],
                                            sizeof(state.path_segments[state.path_depth]),
                                            entry.name);
                                ++state.path_depth;
                                open_current_directory(state);
                                needs_redraw = true;
                            }
                        } else {
                            long handle = file_open_at(state.dir_handle,
                                                       entry.name);
                            directory_close(state.dir_handle);
                            unmap(entries, entries_bytes);
                            result.accepted = handle >= 0;
                            result.handle = (handle >= 0)
                                ? static_cast<uint32_t>(handle)
                                : kInvalidFileHandle;
                            return result;
                        }
                    }
                }
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

        yield();
    }
}

}  // namespace lattice

extern "C" void* memset(void* dest, int value, size_t count) {
    if (dest == nullptr || count == 0) {
        return dest;
    }
    uint8_t* bytes = reinterpret_cast<uint8_t*>(dest);
    uint8_t byte = static_cast<uint8_t>(value);
    for (size_t i = 0; i < count; ++i) {
        bytes[i] = byte;
    }
    return dest;
}
