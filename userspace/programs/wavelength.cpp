#include <stddef.h>
#include <stdint.h>

#include "descriptors.hpp"
#include "font8x8_basic.hpp"
#include "wm_protocol.hpp"
#include "lattice/lattice.hpp"
#include "../crt/syscall.hpp"

namespace {

constexpr const char kRegistryName[] = "wm.registry";

constexpr uint32_t kBaseFontSize = 8;
constexpr uint32_t kBaseIconSize = 30;
constexpr uint32_t kBaseIconGap = 18;
constexpr uint32_t kBaseLabelGap = 4;
constexpr uint32_t kBaseLabelHeight = 8;
constexpr uint32_t kBaseTopBarHeight = 22;
constexpr uint32_t kBaseTopBarPaddingX = 12;
constexpr uint32_t kBaseMenuGap = 16;
constexpr uint32_t kBaseMenuPaddingX = 8;
constexpr uint32_t kBaseMenuItemHeight = 14;
constexpr uint32_t kBaseOriginX = 24;
constexpr uint32_t kBaseOriginY = 16;
constexpr uint32_t kMaxUiScale = 4;

uint32_t g_ui_scale = 1;
uint32_t g_font_size = kBaseFontSize;
uint32_t g_icon_size = kBaseIconSize;
uint32_t g_icon_gap = kBaseIconGap;
uint32_t g_label_gap = kBaseLabelGap;
uint32_t g_label_height = kBaseLabelHeight;
uint32_t g_top_bar_height = kBaseTopBarHeight;
uint32_t g_top_bar_padding_x = kBaseTopBarPaddingX;
uint32_t g_menu_gap = kBaseMenuGap;
uint32_t g_menu_padding_x = kBaseMenuPaddingX;
uint32_t g_menu_item_height = kBaseMenuItemHeight;
uint32_t g_origin_x = kBaseOriginX;
uint32_t g_origin_y = kBaseOriginY;

struct Surface {
    uint8_t* buffer;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t bytes_per_pixel;
    wm::PixelFormat format;
};

struct Icon {
    const char* label;
    const char* exec_path;
    uint32_t x;
    uint32_t y;
    char glyph;
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

uint32_t clamp_ui_scale(uint32_t value) {
    if (value == 0) {
        return 1;
    }
    if (value > kMaxUiScale) {
        return kMaxUiScale;
    }
    return value;
}

uint32_t parse_ui_scale(const char* text, size_t length) {
    if (text == nullptr || length == 0) {
        return 1;
    }
    uint32_t value = 0;
    bool found_digit = false;
    for (size_t i = 0; i < length; ++i) {
        char ch = text[i];
        if (ch >= '0' && ch <= '9') {
            found_digit = true;
            value = value * 10u + static_cast<uint32_t>(ch - '0');
            if (value > kMaxUiScale) {
                value = kMaxUiScale;
                break;
            }
        } else if (found_digit) {
            break;
        }
    }
    if (!found_digit) {
        return 1;
    }
    return clamp_ui_scale(value);
}

uint32_t read_ui_scale() {
    char buffer[16];
    long handle = file_open("config/photon/scale");
    if (handle < 0) {
        handle = file_open("/config/photon/scale");
    }
    if (handle < 0) {
        return 1;
    }
    long read = file_read(static_cast<uint32_t>(handle),
                          buffer,
                          sizeof(buffer));
    file_close(static_cast<uint32_t>(handle));
    if (read <= 0) {
        return 1;
    }
    return parse_ui_scale(buffer, static_cast<size_t>(read));
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

bool spawn_from_mounts(const char* suffix) {
    if (suffix == nullptr || suffix[0] == '\0') {
        return false;
    }
    long pid = child(suffix, nullptr, 0, nullptr);
    if (pid >= 0) {
        return true;
    }
    long dir = directory_open("/");
    if (dir < 0) {
        return false;
    }
    DirEntry entry{};
    char path[160];
    while (directory_read(static_cast<uint32_t>(dir), &entry) > 0) {
        if (entry.name[0] == '\0') {
            continue;
        }
        if (!build_mount_subpath(entry.name, suffix, path, sizeof(path))) {
            continue;
        }
        pid = child(path, nullptr, 0, nullptr);
        if (pid >= 0) {
            directory_close(static_cast<uint32_t>(dir));
            return true;
        }
    }
    directory_close(static_cast<uint32_t>(dir));
    return false;
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

void send_menu_invoke(uint32_t handle, uint8_t menu_index, uint8_t item_index) {
    if (handle == kInvalidDescriptor) {
        return;
    }
    wm::ClientMenuInvoke msg{};
    msg.type = static_cast<uint8_t>(wm::ClientMessage::MenuInvoke);
    msg.menu_index = menu_index;
    msg.item_index = item_index;
    msg.reserved = 0;
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
    uint32_t scale = g_ui_scale;
    if (scale == 0) {
        scale = 1;
    }
    for (uint32_t row = 0; row < kBaseFontSize; ++row) {
        uint8_t bits = font8x8_basic[uc][row];
        int32_t py = y + static_cast<int32_t>(row * scale);
        for (uint32_t col = 0; col < kBaseFontSize; ++col) {
            if ((bits & (1u << col)) == 0) {
                continue;
            }
            int32_t px = x + static_cast<int32_t>(col * scale);
            for (uint32_t sy = 0; sy < scale; ++sy) {
                int32_t draw_y = py + static_cast<int32_t>(sy);
                if (draw_y < 0 ||
                    draw_y >= static_cast<int32_t>(surface.height)) {
                    continue;
                }
                for (uint32_t sx = 0; sx < scale; ++sx) {
                    int32_t draw_x = px + static_cast<int32_t>(sx);
                    if (draw_x < 0 ||
                        draw_x >= static_cast<int32_t>(surface.width)) {
                        continue;
                    }
                    lattice::write_pixel(surface.buffer,
                                         surface.stride,
                                         surface.bytes_per_pixel,
                                         static_cast<uint32_t>(draw_x),
                                         static_cast<uint32_t>(draw_y),
                                         color);
                }
            }
        }
    }
}

void draw_text(const Surface& surface,
               int32_t x,
               int32_t y,
               const char* text,
               uint32_t color) {
    if (text == nullptr) {
        return;
    }
    int32_t cursor = x;
    for (size_t i = 0; text[i] != '\0'; ++i) {
        draw_char(surface, cursor, y, text[i], color);
        cursor += static_cast<int32_t>(g_font_size);
    }
}

uint32_t text_width(const char* text) {
    if (text == nullptr) {
        return 0;
    }
    size_t len = 0;
    while (text[len] != '\0') {
        ++len;
    }
    return static_cast<uint32_t>(len) * g_font_size;
}

struct MenuLayout {
    uint32_t x[wm::kMenuMaxMenus];
    uint32_t width[wm::kMenuMaxMenus];
    uint32_t count;
};

void layout_menus(const wm::MenuBar& bar, uint32_t start_x, MenuLayout& layout) {
    layout.count = bar.menu_count;
    if (layout.count > wm::kMenuMaxMenus) {
        layout.count = wm::kMenuMaxMenus;
    }
    uint32_t cursor = start_x;
    for (uint32_t i = 0; i < layout.count; ++i) {
        layout.x[i] = cursor;
        layout.width[i] = text_width(bar.menus[i].label);
        cursor += layout.width[i] + g_menu_gap;
    }
}

void clamp_menu_bar(wm::MenuBar& bar) {
    if (bar.menu_count > wm::kMenuMaxMenus) {
        bar.menu_count = wm::kMenuMaxMenus;
    }
    for (uint8_t i = 0; i < bar.menu_count; ++i) {
        if (bar.menus[i].item_count > wm::kMenuMaxItems) {
            bar.menus[i].item_count = wm::kMenuMaxItems;
        }
    }
}

uint32_t menu_popup_width(const wm::Menu& menu) {
    uint32_t max_width = 0;
    uint8_t count = menu.item_count;
    if (count > wm::kMenuMaxItems) {
        count = wm::kMenuMaxItems;
    }
    for (uint8_t i = 0; i < count; ++i) {
        uint32_t w = text_width(menu.items[i].label);
        if (w > max_width) {
            max_width = w;
        }
    }
    return max_width + g_menu_padding_x * 2;
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

void draw_icon(const Surface& surface,
               const Icon& icon,
               uint32_t fill,
               uint32_t border,
               uint32_t label) {
    fill_rect(surface,
              static_cast<int32_t>(icon.x),
              static_cast<int32_t>(icon.y),
              g_icon_size,
              g_icon_size,
              fill);
    fill_rect(surface,
              static_cast<int32_t>(icon.x),
              static_cast<int32_t>(icon.y),
              g_icon_size,
              1,
              border);
    fill_rect(surface,
              static_cast<int32_t>(icon.x),
              static_cast<int32_t>(icon.y + g_icon_size - 1),
              g_icon_size,
              1,
              border);
    fill_rect(surface,
              static_cast<int32_t>(icon.x),
              static_cast<int32_t>(icon.y),
              1,
              g_icon_size,
              border);
    fill_rect(surface,
              static_cast<int32_t>(icon.x + g_icon_size - 1),
              static_cast<int32_t>(icon.y),
              1,
              g_icon_size,
              border);

    if (icon.glyph != '\0') {
        int32_t glyph_x =
            static_cast<int32_t>(icon.x +
                                 (g_icon_size - g_font_size) / 2);
        int32_t glyph_y =
            static_cast<int32_t>(icon.y +
                                 (g_icon_size - g_font_size) / 2);
        draw_char(surface, glyph_x, glyph_y, icon.glyph, label);
    }

    uint32_t label_y = icon.y + g_icon_size + g_label_gap;
    uint32_t label_width = text_width(icon.label);
    int32_t label_x = static_cast<int32_t>(icon.x);
    if (label_width != 0) {
        if (label_width <= g_icon_size) {
            label_x = static_cast<int32_t>(icon.x) +
                      static_cast<int32_t>((g_icon_size - label_width) / 2);
        } else {
            label_x = static_cast<int32_t>(icon.x) -
                      static_cast<int32_t>((label_width - g_icon_size) / 2);
        }
    }
    draw_text(surface,
              label_x,
              static_cast<int32_t>(label_y),
              icon.label,
              label);
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
    request.width = 0;
    request.height = 0;
    request.flags = wm::kWindowFlagBackground;
    copy_string(request.title, sizeof(request.title), "Wavelength");

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

    g_ui_scale = clamp_ui_scale(read_ui_scale());
    g_font_size = kBaseFontSize * g_ui_scale;
    g_icon_size = kBaseIconSize * g_ui_scale;
    g_icon_gap = kBaseIconGap * g_ui_scale;
    g_label_gap = kBaseLabelGap * g_ui_scale;
    g_label_height = kBaseLabelHeight * g_ui_scale;
    g_top_bar_height = kBaseTopBarHeight * g_ui_scale;
    g_top_bar_padding_x = kBaseTopBarPaddingX * g_ui_scale;
    g_menu_gap = kBaseMenuGap * g_ui_scale;
    g_menu_padding_x = kBaseMenuPaddingX * g_ui_scale;
    g_menu_item_height = kBaseMenuItemHeight * g_ui_scale;
    g_origin_x = kBaseOriginX * g_ui_scale;
    g_origin_y = kBaseOriginY * g_ui_scale;

    uint32_t bg = lattice::pack_color(surface.format, 20, 24, 32);
    uint32_t topbar_bg = lattice::pack_color(surface.format, 28, 32, 40);
    uint32_t topbar_border = lattice::pack_color(surface.format, 10, 12, 18);
    uint32_t menu_active_bg = lattice::pack_color(surface.format, 38, 44, 56);
    uint32_t menu_panel_bg = lattice::pack_color(surface.format, 30, 34, 42);
    uint32_t menu_panel_border = lattice::pack_color(surface.format, 12, 14, 20);
    uint32_t icon_fill = lattice::pack_color(surface.format, 78, 110, 190);
    uint32_t icon_border = lattice::pack_color(surface.format, 16, 18, 26);
    uint32_t label_color = lattice::pack_color(surface.format, 230, 235, 245);

    wm::MenuBar menu_bar{};
    char focused_title[32];
    copy_string(focused_title, sizeof(focused_title), "Wavelength");
    int active_menu = -1;

    Icon icons[] = {
        {"Ion", "binary/ion.elf", 0, 0, 'I'},
        {"Orbital", "binary/orbital.elf", 2, 0, 'O'},
    };
    size_t icon_count = sizeof(icons) / sizeof(icons[0]);

    uint32_t cell_width = g_icon_size + g_icon_gap;
    uint32_t cell_height =
        g_icon_size + g_label_gap + g_label_height + g_icon_gap;
    uint32_t origin_x = g_origin_x;
    uint32_t origin_y = g_top_bar_height + g_origin_y;
    uint32_t usable_width =
        (surface.width > origin_x) ? surface.width - origin_x : surface.width;
    uint32_t columns = (cell_width > 0) ? (usable_width / cell_width) : 1;
    if (columns == 0) {
        columns = 1;
    }
    for (size_t i = 0; i < icon_count; ++i) {
        uint32_t col = static_cast<uint32_t>(i) % columns;
        uint32_t row = static_cast<uint32_t>(i) / columns;
        icons[i].x = origin_x + col * cell_width;
        icons[i].y = origin_y + row * cell_height;
    }

    auto render_scene = [&]() {
        fill_rect(surface, 0, 0, surface.width, surface.height, bg);

        fill_rect(surface, 0, 0, surface.width, g_top_bar_height, topbar_bg);
        fill_rect(surface,
                  0,
                  static_cast<int32_t>(g_top_bar_height - 1),
                  surface.width,
                  1,
                  topbar_border);
        uint32_t title_width = text_width(focused_title);
        int32_t title_x = static_cast<int32_t>(g_top_bar_padding_x);
        int32_t title_y =
            static_cast<int32_t>((g_top_bar_height - g_label_height) / 2);
        draw_text(surface, title_x, title_y, focused_title, label_color);

        uint32_t menu_start = g_top_bar_padding_x + title_width + g_menu_gap;
        if (menu_start < g_top_bar_padding_x + g_menu_gap) {
            menu_start = g_top_bar_padding_x + g_menu_gap;
        }
        MenuLayout layout{};
        layout_menus(menu_bar, menu_start, layout);
        for (uint32_t i = 0; i < layout.count; ++i) {
            if (static_cast<int>(i) == active_menu) {
                int32_t highlight_x =
                    static_cast<int32_t>(layout.x[i]) -
                    static_cast<int32_t>(g_menu_padding_x);
                fill_rect(surface,
                          highlight_x,
                          0,
                          layout.width[i] + g_menu_padding_x * 2,
                          g_top_bar_height,
                          menu_active_bg);
            }
            int32_t menu_y =
                static_cast<int32_t>((g_top_bar_height - g_label_height) / 2);
            draw_text(surface,
                      static_cast<int32_t>(layout.x[i]),
                      menu_y,
                      menu_bar.menus[i].label,
                      label_color);
        }

        if (active_menu >= 0 &&
            active_menu < static_cast<int>(layout.count)) {
            const wm::Menu& menu = menu_bar.menus[active_menu];
            uint8_t item_count = menu.item_count;
            if (item_count > wm::kMenuMaxItems) {
                item_count = wm::kMenuMaxItems;
            }
            uint32_t panel_width = menu_popup_width(menu);
            uint32_t panel_height =
                static_cast<uint32_t>(item_count) * g_menu_item_height +
                g_menu_padding_x * 2;
            int32_t panel_x =
                static_cast<int32_t>(layout.x[active_menu]) -
                static_cast<int32_t>(g_menu_padding_x);
            int32_t panel_y = static_cast<int32_t>(g_top_bar_height);
            fill_rect(surface,
                      panel_x,
                      panel_y,
                      panel_width,
                      panel_height,
                      menu_panel_bg);
            fill_rect(surface,
                      panel_x,
                      panel_y,
                      panel_width,
                      1,
                      menu_panel_border);
            fill_rect(surface,
                      panel_x,
                      panel_y,
                      1,
                      panel_height,
                      menu_panel_border);
            fill_rect(surface,
                      panel_x,
                      panel_y + static_cast<int32_t>(panel_height - 1),
                      panel_width,
                      1,
                      menu_panel_border);
            fill_rect(surface,
                      panel_x + static_cast<int32_t>(panel_width - 1),
                      panel_y,
                      1,
                      panel_height,
                      menu_panel_border);
            for (uint8_t i = 0; i < item_count; ++i) {
                int32_t text_x =
                    panel_x + static_cast<int32_t>(g_menu_padding_x);
                int32_t text_y =
                    panel_y +
                    static_cast<int32_t>(g_menu_padding_x) +
                    static_cast<int32_t>(i * g_menu_item_height);
                draw_text(surface,
                          text_x,
                          text_y,
                          menu.items[i].label,
                          label_color);
            }
        }

        for (size_t i = 0; i < icon_count; ++i) {
            draw_icon(surface, icons[i], icon_fill, icon_border, label_color);
        }
    };

    render_scene();
    if (present_handle != kInvalidDescriptor) {
        uint8_t msg = static_cast<uint8_t>(wm::ClientMessage::Present);
        write_pipe_all(present_handle, &msg, sizeof(msg));
    }

    uint8_t buffer[2048];
    size_t pending = 0;

    while (1) {
        long read = descriptor_read(static_cast<uint32_t>(reply_handle),
                                    buffer + pending,
                                    sizeof(buffer) - pending);
        if (read > 0) {
            pending += static_cast<size_t>(read);
        }

        bool needs_redraw = false;
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
                wm::ServerMouseMessage msg{};
                lattice::copy_bytes(reinterpret_cast<uint8_t*>(&msg),
                                    buffer + offset,
                                    sizeof(msg));
                offset += sizeof(msg);
                bool left = (msg.buttons & 0x1u) != 0;
                if (left) {
                    bool handled = false;
                    uint32_t title_width = text_width(focused_title);
                    uint32_t menu_start =
                        g_top_bar_padding_x + title_width + g_menu_gap;
                    if (menu_start < g_top_bar_padding_x + g_menu_gap) {
                        menu_start = g_top_bar_padding_x + g_menu_gap;
                    }
                    MenuLayout layout{};
                    layout_menus(menu_bar, menu_start, layout);

                    if (active_menu >= 0 &&
                        active_menu < static_cast<int>(layout.count)) {
                        const wm::Menu& menu = menu_bar.menus[active_menu];
                        uint8_t item_count = menu.item_count;
                        if (item_count > wm::kMenuMaxItems) {
                            item_count = wm::kMenuMaxItems;
                        }
                        uint32_t panel_width = menu_popup_width(menu);
                        uint32_t panel_height =
                            static_cast<uint32_t>(item_count) * g_menu_item_height +
                            g_menu_padding_x * 2;
                        int32_t panel_x =
                            static_cast<int32_t>(layout.x[active_menu]) -
                            static_cast<int32_t>(g_menu_padding_x);
                        int32_t panel_y =
                            static_cast<int32_t>(g_top_bar_height);
                        if (point_in_rect(msg.x, msg.y,
                                          static_cast<uint32_t>(panel_x),
                                          static_cast<uint32_t>(panel_y),
                                          panel_width,
                                          panel_height)) {
                            uint32_t rel_y =
                                msg.y - static_cast<uint32_t>(panel_y);
                            if (rel_y >= g_menu_padding_x) {
                                uint32_t index =
                                    (rel_y - g_menu_padding_x) /
                                    g_menu_item_height;
                                if (index < item_count) {
                                    send_menu_invoke(present_handle,
                                                     static_cast<uint8_t>(active_menu),
                                                     static_cast<uint8_t>(index));
                                }
                            }
                            active_menu = -1;
                            needs_redraw = true;
                            handled = true;
                        }
                    }

                    if (!handled && msg.y < g_top_bar_height) {
                        for (uint32_t i = 0; i < layout.count; ++i) {
                            uint32_t hit_x = layout.x[i] >= g_menu_padding_x
                                ? layout.x[i] - g_menu_padding_x
                                : 0;
                            uint32_t hit_w =
                                layout.width[i] + g_menu_padding_x * 2;
                            if (point_in_rect(msg.x, msg.y,
                                              hit_x,
                                              0,
                                              hit_w,
                                              g_top_bar_height)) {
                                if (active_menu == static_cast<int>(i)) {
                                    active_menu = -1;
                                } else {
                                    active_menu = static_cast<int>(i);
                                }
                                needs_redraw = true;
                                handled = true;
                                break;
                            }
                        }
                    }

                    if (!handled) {
                        if (active_menu >= 0) {
                            active_menu = -1;
                            needs_redraw = true;
                            handled = true;
                        }
                    }

                    if (!handled && msg.y >= g_top_bar_height) {
                        for (size_t i = 0; i < icon_count; ++i) {
                            if (point_in_rect(msg.x, msg.y,
                                              icons[i].x,
                                              icons[i].y,
                                              g_icon_size,
                                              g_icon_size)) {
                                spawn_from_mounts(icons[i].exec_path);
                                break;
                            }
                        }
                    }
                }
                continue;
            }
            if (type == static_cast<uint8_t>(wm::ServerMessage::Key)) {
                if (pending - offset < sizeof(wm::ServerKeyMessage)) {
                    break;
                }
                offset += sizeof(wm::ServerKeyMessage);
                continue;
            }
            if (type == static_cast<uint8_t>(wm::ServerMessage::MenuBar)) {
                if (pending - offset < sizeof(wm::ServerMenuBarMessage)) {
                    break;
                }
                wm::ServerMenuBarMessage msg{};
                lattice::copy_bytes(reinterpret_cast<uint8_t*>(&msg),
                                    buffer + offset,
                                    sizeof(msg));
                copy_string(focused_title, sizeof(focused_title), msg.title);
                menu_bar = msg.bar;
                clamp_menu_bar(menu_bar);
                active_menu = -1;
                needs_redraw = true;
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

        if (needs_redraw) {
            render_scene();
            if (present_handle != kInvalidDescriptor) {
                uint8_t msg = static_cast<uint8_t>(wm::ClientMessage::Present);
                write_pipe_all(present_handle, &msg, sizeof(msg));
            }
        }

        yield();
    }
}
