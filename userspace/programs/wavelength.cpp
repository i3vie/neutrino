#include <stddef.h>
#include <stdint.h>

#include "descriptors.hpp"
#include "font8x8_basic.hpp"
#include "wm_protocol.hpp"
#include "lattice/lattice.hpp"
#include "../crt/syscall.hpp"

namespace {

constexpr const char kRegistryName[] = "wm.registry";

constexpr uint32_t kIconSize = 30;
constexpr uint32_t kIconGap = 18;
constexpr uint32_t kLabelGap = 4;
constexpr uint32_t kLabelHeight = 8;
constexpr uint32_t kTopBarHeight = 22;
constexpr uint32_t kTopBarPaddingX = 12;

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
    for (uint32_t row = 0; row < 8; ++row) {
        uint8_t bits = font8x8_basic[uc][row];
        int32_t py = y + static_cast<int32_t>(row);
        if (py < 0 || py >= static_cast<int32_t>(surface.height)) {
            continue;
        }
        for (uint32_t col = 0; col < 8; ++col) {
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
        cursor += 8;
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
    return static_cast<uint32_t>(len) * 8u;
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
              kIconSize,
              kIconSize,
              fill);
    fill_rect(surface,
              static_cast<int32_t>(icon.x),
              static_cast<int32_t>(icon.y),
              kIconSize,
              1,
              border);
    fill_rect(surface,
              static_cast<int32_t>(icon.x),
              static_cast<int32_t>(icon.y + kIconSize - 1),
              kIconSize,
              1,
              border);
    fill_rect(surface,
              static_cast<int32_t>(icon.x),
              static_cast<int32_t>(icon.y),
              1,
              kIconSize,
              border);
    fill_rect(surface,
              static_cast<int32_t>(icon.x + kIconSize - 1),
              static_cast<int32_t>(icon.y),
              1,
              kIconSize,
              border);

    if (icon.glyph != '\0') {
        int32_t glyph_x =
            static_cast<int32_t>(icon.x + (kIconSize - 8) / 2);
        int32_t glyph_y =
            static_cast<int32_t>(icon.y + (kIconSize - 8) / 2);
        draw_char(surface, glyph_x, glyph_y, icon.glyph, label);
    }

    uint32_t label_y = icon.y + kIconSize + kLabelGap;
    uint32_t label_width = text_width(icon.label);
    int32_t label_x = static_cast<int32_t>(icon.x);
    if (label_width != 0) {
        if (label_width <= kIconSize) {
            label_x = static_cast<int32_t>(icon.x) +
                      static_cast<int32_t>((kIconSize - label_width) / 2);
        } else {
            label_x = static_cast<int32_t>(icon.x) -
                      static_cast<int32_t>((label_width - kIconSize) / 2);
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

    uint32_t bg = lattice::pack_color(surface.format, 20, 24, 32);
    uint32_t topbar_bg = lattice::pack_color(surface.format, 28, 32, 40);
    uint32_t topbar_border = lattice::pack_color(surface.format, 10, 12, 18);
    uint32_t icon_fill = lattice::pack_color(surface.format, 78, 110, 190);
    uint32_t icon_border = lattice::pack_color(surface.format, 16, 18, 26);
    uint32_t label_color = lattice::pack_color(surface.format, 230, 235, 245);

    fill_rect(surface, 0, 0, surface.width, surface.height, bg);

    fill_rect(surface, 0, 0, surface.width, kTopBarHeight, topbar_bg);
    fill_rect(surface,
              0,
              static_cast<int32_t>(kTopBarHeight - 1),
              surface.width,
              1,
              topbar_border);
    uint32_t title_width = text_width("Wavelength");
    int32_t title_x = static_cast<int32_t>(kTopBarPaddingX);
    if (title_width < surface.width) {
        title_x = static_cast<int32_t>(
            (surface.width - title_width) / 2);
    }
    int32_t title_y =
        static_cast<int32_t>((kTopBarHeight - kLabelHeight) / 2);
    draw_text(surface, title_x, title_y, "Wavelength", label_color);

    Icon icons[] = {
        {"Terminal", "binary/ion.elf", 0, 0, 'T'},
    };

    uint32_t cell_width = kIconSize + kIconGap;
    uint32_t cell_height = kIconSize + kLabelGap + kLabelHeight + kIconGap;
    uint32_t origin_x = 24;
    uint32_t origin_y = kTopBarHeight + 16;
    uint32_t usable_width =
        (surface.width > origin_x) ? surface.width - origin_x : surface.width;
    uint32_t columns = (cell_width > 0) ? (usable_width / cell_width) : 1;
    if (columns == 0) {
        columns = 1;
    }
    for (size_t i = 0; i < sizeof(icons) / sizeof(icons[0]); ++i) {
        uint32_t col = static_cast<uint32_t>(i) % columns;
        uint32_t row = static_cast<uint32_t>(i) / columns;
        icons[i].x = origin_x + col * cell_width;
        icons[i].y = origin_y + row * cell_height;
    }

    for (size_t i = 0; i < sizeof(icons) / sizeof(icons[0]); ++i) {
        draw_icon(surface, icons[i], icon_fill, icon_border, label_color);
    }

    if (present_handle != kInvalidDescriptor) {
        uint8_t msg = static_cast<uint8_t>(wm::ClientMessage::Present);
        write_pipe_all(present_handle, &msg, sizeof(msg));
    }

    uint8_t buffer[64];
    size_t pending = 0;

    while (1) {
        long read = descriptor_read(static_cast<uint32_t>(reply_handle),
                                    buffer + pending,
                                    sizeof(buffer) - pending);
        if (read > 0) {
            pending += static_cast<size_t>(read);
        }

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
                if ((msg.buttons & 0x1u) != 0) {
                    for (size_t i = 0; i < sizeof(icons) / sizeof(icons[0]); ++i) {
                        if (point_in_rect(msg.x, msg.y,
                                          icons[i].x,
                                          icons[i].y,
                                          kIconSize,
                                          kIconSize)) {
                            spawn_from_mounts(icons[i].exec_path);
                            break;
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
