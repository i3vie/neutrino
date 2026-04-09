#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "descriptors.hpp"
#include "font8x8_basic.hpp"
#include "keyboard_scancode.hpp"
#include "lattice/lattice.hpp"
#include "wm_protocol.hpp"
#include "../crt/syscall.hpp"

namespace {

constexpr const char kRegistryName[] = "wm.registry";
constexpr const char kProgramTitle[] = "Flux";
constexpr uint32_t kWindowWidth = 520;
constexpr uint32_t kWindowHeight = 388;
constexpr uint32_t kToolbarHeight = 28;
constexpr uint32_t kCanvasPadding = 12;
constexpr uint32_t kPaletteHeight = 64;
constexpr uint32_t kPalettePadding = 8;
constexpr uint32_t kPaletteSquare = 30;
constexpr uint32_t kPaletteCount = 8;
constexpr uint32_t kBrushMin = 1;
constexpr uint32_t kBrushMax = 14;
constexpr uint32_t kDefaultBrush = 4;
constexpr uint32_t kBaseFontSize = 8;
constexpr size_t kMaxCanvasPixels =
    static_cast<size_t>(kWindowWidth) * kWindowHeight;
constexpr size_t kMaxBmpBuffer = kMaxCanvasPixels * 3 + 4096;
constexpr size_t kMaxRowStride = kMaxBmpBuffer;
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

struct ColorRGB {
    uint8_t r;
    uint8_t g;
    uint8_t b;
};
constexpr ColorRGB kPaletteRGB[kPaletteCount] = {
    {229, 57, 53},   // red
    {255, 143, 0},   // orange
    {255, 235, 59},  // yellow
    {64, 198, 68},   // green
    {3, 169, 244},   // cyan
    {124, 77, 255},  // purple
    {255, 255, 255}, // white
    {33, 33, 33},    // black
};

uint32_t g_font_size = kBaseFontSize;
uint8_t extract_component(uint32_t pixel, uint8_t mask_size, uint8_t mask_shift) {
    if (mask_size == 0) {
        return 0;
    }
    uint32_t mask = (mask_size >= 32) ? 0xFFFFFFFFu : ((1u << mask_size) - 1u);
    uint32_t value = (pixel >> mask_shift) & mask;
    if (mask == 0) {
        return 0;
    }
    uint32_t scaled = (value * 255u) / mask;
    return static_cast<uint8_t>(scaled);
}

ColorRGB unpack_color(uint32_t pixel, const wm::PixelFormat& fmt) {
    ColorRGB out{};
    out.r = extract_component(pixel, fmt.red_mask_size, fmt.red_mask_shift);
    out.g = extract_component(pixel, fmt.green_mask_size, fmt.green_mask_shift);
    out.b = extract_component(pixel, fmt.blue_mask_size, fmt.blue_mask_shift);
    return out;
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

size_t append_string(char* dest,
                     size_t dest_size,
                     size_t offset,
                     const char* src) {
    if (dest == nullptr || dest_size == 0 || offset >= dest_size) {
        return offset;
    }
    size_t limit = dest_size - 1;
    size_t idx = 0;
    while (src != nullptr && src[idx] != '\0' && offset < limit) {
        dest[offset++] = src[idx++];
    }
    dest[offset] = '\0';
    return offset;
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

size_t append_uint32(char* dest,
                     size_t dest_size,
                     size_t offset,
                     uint32_t value) {
    if (dest == nullptr || dest_size == 0 || offset >= dest_size) {
        return offset;
    }
    size_t limit = dest_size - 1;
    char digits[16];
    size_t count = 0;
    if (value == 0) {
        digits[count++] = '0';
    } else {
        while (value != 0 && count < sizeof(digits)) {
            digits[count++] = static_cast<char>('0' + (value % 10));
            value /= 10;
        }
    }
    while (count > 0 && offset < limit) {
        dest[offset++] = digits[--count];
    }
    dest[offset] = '\0';
    return offset;
}

void store_u32_le(uint8_t* dest, uint32_t value) {
    dest[0] = static_cast<uint8_t>(value & 0xFFu);
    dest[1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
    dest[2] = static_cast<uint8_t>((value >> 16) & 0xFFu);
    dest[3] = static_cast<uint8_t>((value >> 24) & 0xFFu);
}

bool save_canvas_bmp_handle(uint32_t handle,
                            const wm::PixelFormat& fmt,
                            const uint32_t* pixels,
                            uint32_t width,
                            uint32_t height) {
    if (handle == kInvalidDescriptor ||
        pixels == nullptr || width == 0 || height == 0) {
        return false;
    }
    size_t row_stride = (static_cast<size_t>(width) * 3u + 3u) & ~static_cast<size_t>(3u);
    size_t pixel_bytes = row_stride * height;
    size_t file_size = 14 + 40 + pixel_bytes;
    uint8_t header[54]{};
    header[0] = 'B';
    header[1] = 'M';
    store_u32_le(header + 2, static_cast<uint32_t>(file_size));
    store_u32_le(header + 10, 54u);
    store_u32_le(header + 14, 40u);
    store_u32_le(header + 18, width);
    store_u32_le(header + 22, height);
    header[26] = 1u;
    header[28] = 24u;
    store_u32_le(header + 34, static_cast<uint32_t>(pixel_bytes));

    bool ok = file_write(handle, header, sizeof(header)) == static_cast<long>(sizeof(header));
    if (!ok) {
        return false;
    }

    size_t max_row_bytes = static_cast<size_t>(kWindowWidth) * 3u + 4u;
    uint8_t row_buffer[max_row_bytes];
    if (row_stride > max_row_bytes) {
        return false;
    }
    for (int32_t row = static_cast<int32_t>(height) - 1; row >= 0; --row) {
        size_t dst = 0;
        for (uint32_t col = 0; col < width; ++col) {
            uint32_t pixel = pixels[static_cast<size_t>(row) * width + col];
            ColorRGB rgb = unpack_color(pixel, fmt);
            row_buffer[dst++] = rgb.b;
            row_buffer[dst++] = rgb.g;
            row_buffer[dst++] = rgb.r;
        }
        while (dst < row_stride) {
            row_buffer[dst++] = 0;
        }
        long written = file_write(static_cast<uint32_t>(handle),
                                  row_buffer,
                                  row_stride);
        if (written != static_cast<long>(row_stride)) {
            ok = false;
            break;
        }
        if ((row & 0xFu) == 0) {
            yield();  // let other tasks run during long saves
        }
    }
    return ok;
}

bool save_canvas_bmp(const char* path,
                     const wm::PixelFormat& fmt,
                     const uint32_t* pixels,
                     uint32_t width,
                     uint32_t height) {
    if (path == nullptr || pixels == nullptr ||
        width == 0 || height == 0) {
        return false;
    }
    long handle = file_create(path);
    if (handle < 0) {
        return false;
    }
    bool ok = save_canvas_bmp_handle(static_cast<uint32_t>(handle),
                                     fmt,
                                     pixels,
                                     width,
                                     height);
    file_close(static_cast<uint32_t>(handle));
    return ok;
}

bool load_canvas_bmp_handle(uint32_t handle,
                            const wm::PixelFormat& fmt,
                            uint32_t* pixels,
                            uint32_t canvas_width,
                            uint32_t canvas_height,
                            uint32_t bg_color) {
    if (handle == kInvalidDescriptor || pixels == nullptr ||
        canvas_width == 0 || canvas_height == 0) {
        return false;
    }
    uint8_t header[54];
    size_t header_read = 0;
    while (header_read < sizeof(header)) {
        long got = file_read(handle, header + header_read, sizeof(header) - header_read);
        if (got <= 0) {
            return false;
        }
        header_read += static_cast<size_t>(got);
    }
    if (header[0] != 'B' || header[1] != 'M') {
        return false;
    }
    uint32_t data_offset = header[10] |
                           (static_cast<uint32_t>(header[11]) << 8) |
                           (static_cast<uint32_t>(header[12]) << 16) |
                           (static_cast<uint32_t>(header[13]) << 24);
    uint32_t dib_size = header[14] |
                        (static_cast<uint32_t>(header[15]) << 8) |
                        (static_cast<uint32_t>(header[16]) << 16) |
                        (static_cast<uint32_t>(header[17]) << 24);
    if (dib_size < 40 || data_offset < sizeof(header)) {
        return false;
    }
    uint32_t img_width = header[18] |
                         (static_cast<uint32_t>(header[19]) << 8) |
                         (static_cast<uint32_t>(header[20]) << 16) |
                         (static_cast<uint32_t>(header[21]) << 24);
    uint32_t img_height = header[22] |
                          (static_cast<uint32_t>(header[23]) << 8) |
                          (static_cast<uint32_t>(header[24]) << 16) |
                          (static_cast<uint32_t>(header[25]) << 24);
    uint16_t planes = static_cast<uint16_t>(header[26] | (header[27] << 8));
    uint16_t bpp = static_cast<uint16_t>(header[28] | (header[29] << 8));
    uint32_t compression = header[30] |
                           (static_cast<uint32_t>(header[31]) << 8) |
                           (static_cast<uint32_t>(header[32]) << 16) |
                           (static_cast<uint32_t>(header[33]) << 24);
    if (planes != 1 || bpp != 24 || compression != 0 ||
        img_width == 0 || img_height == 0) {
        return false;
    }
    size_t row_stride = (static_cast<size_t>(img_width) * 3u + 3u) & ~static_cast<size_t>(3u);
    if (row_stride == 0 || row_stride > kMaxRowStride) {
        return false;
    }

    // Skip any remaining header/padding up to pixel data.
    uint32_t to_skip = data_offset - static_cast<uint32_t>(sizeof(header));
    uint8_t skip_buf[64];
    while (to_skip > 0) {
        size_t chunk = (to_skip > sizeof(skip_buf)) ? sizeof(skip_buf) : to_skip;
        long read = file_read(handle, skip_buf, chunk);
        if (read <= 0) {
            return false;
        }
        to_skip -= static_cast<uint32_t>(read);
    }

    size_t total = static_cast<size_t>(canvas_width) * canvas_height;
    for (size_t i = 0; i < total; ++i) {
        pixels[i] = bg_color;
    }

    uint32_t copy_width = (img_width < canvas_width) ? img_width : canvas_width;
    uint32_t copy_height = (img_height < canvas_height) ? img_height : canvas_height;

    static uint8_t row_buffer[kMaxRowStride];
    for (uint32_t row = 0; row < copy_height; ++row) {
        size_t filled = 0;
        while (filled < row_stride) {
            long got = file_read(handle,
                                 row_buffer + filled,
                                 row_stride - filled);
            if (got <= 0) {
                return false;
            }
            filled += static_cast<size_t>(got);
        }
        size_t dest_row = copy_height - 1 - row;
        for (uint32_t col = 0; col < copy_width; ++col) {
            size_t src = static_cast<size_t>(col) * 3u;
            uint8_t b = row_buffer[src + 0];
            uint8_t g = row_buffer[src + 1];
            uint8_t r = row_buffer[src + 2];
            uint32_t packed = lattice::pack_color(fmt, r, g, b);
            pixels[dest_row * canvas_width + col] = packed;
        }
        // Skip padding bytes beyond copy_width.
        if (row_stride > static_cast<size_t>(copy_width) * 3u) {
            // nothing to do; already read into row_buffer
        }
        if ((row & 0xFu) == 0) {
            yield();  // keep the UI responsive during long loads
        }
    }
    return true;
}

bool load_canvas_bmp(const char* path,
                     const wm::PixelFormat& fmt,
                     uint32_t* pixels,
                     uint32_t canvas_width,
                     uint32_t canvas_height,
                     uint32_t bg_color) {
    if (path == nullptr || pixels == nullptr ||
        canvas_width == 0 || canvas_height == 0) {
        return false;
    }
    long handle = file_open(path);
    if (handle < 0) {
        return false;
    }
    bool ok = load_canvas_bmp_handle(static_cast<uint32_t>(handle),
                                     fmt,
                                     pixels,
                                     canvas_width,
                                     canvas_height,
                                     bg_color);
    file_close(static_cast<uint32_t>(handle));
    return ok;
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
    if (surface.buffer == nullptr) {
        return;
    }
    uint8_t uc = static_cast<uint8_t>(ch);
    if (uc >= 128) {
        uc = static_cast<uint8_t>('?');
    }
    uint32_t scale = (g_font_size / kBaseFontSize);
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
                if (draw_y < 0 || draw_y >= static_cast<int32_t>(surface.height)) {
                    continue;
                }
                for (uint32_t sx = 0; sx < scale; ++sx) {
                    int32_t draw_x = px + static_cast<int32_t>(sx);
                    if (draw_x < 0 || draw_x >= static_cast<int32_t>(surface.width)) {
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

bool point_in_rect(uint16_t px,
                   uint16_t py,
                   uint32_t x,
                   uint32_t y,
                   uint32_t width,
                   uint32_t height) {
    return px >= x && py >= y && px < x + width && py < y + height;
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

void send_present(uint32_t handle) {
    if (handle == kInvalidDescriptor) {
        return;
    }
    uint8_t msg = static_cast<uint8_t>(wm::ClientMessage::Present);
    write_pipe_all(handle, &msg, sizeof(msg));
}

void send_present_rect(uint32_t handle,
                       uint16_t x,
                       uint16_t y,
                       uint16_t width,
                       uint16_t height) {
    if (handle == kInvalidDescriptor || width == 0 || height == 0) {
        return;
    }
    wm::ClientPresentRect msg{};
    msg.type = static_cast<uint8_t>(wm::ClientMessage::PresentRect);
    msg.x = x;
    msg.y = y;
    msg.width = width;
    msg.height = height;
    write_pipe_all(handle, &msg, sizeof(msg));
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
    wm::Registry* registry = reinterpret_cast<wm::Registry*>(registry_info.base);
    while (registry->magic != wm::kRegistryMagic ||
           registry->version != wm::kRegistryVersion ||
           registry->server_pipe_id == 0) {
        yield();
    }

    uint64_t reply_flags = static_cast<uint64_t>(descriptor_defs::Flag::Readable) |
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

    uint64_t server_flags = static_cast<uint64_t>(descriptor_defs::Flag::Writable) |
                            static_cast<uint64_t>(descriptor_defs::Flag::Async);
    long server_pipe = pipe_open_existing(server_flags, registry->server_pipe_id);
    if (server_pipe < 0) {
        descriptor_close(static_cast<uint32_t>(reply_handle));
        return 1;
    }

    wm::CreateRequest request{};
    request.type = static_cast<uint32_t>(wm::MessageType::CreateWindow);
    request.reply_pipe_id = reply_info.id;
    request.width = kWindowWidth;
    request.height = kWindowHeight;
    request.flags = 0;
    copy_string(request.title, sizeof(request.title), kProgramTitle);

    if (!write_pipe_all(static_cast<uint32_t>(server_pipe),
                        &request,
                        sizeof(request))) {
        descriptor_close(static_cast<uint32_t>(server_pipe));
        descriptor_close(static_cast<uint32_t>(reply_handle));
        return 1;
    }
    descriptor_close(static_cast<uint32_t>(server_pipe));

    wm::CreateResponse response{};
    if (!read_pipe_exact(static_cast<uint32_t>(reply_handle),
                         &response,
                         sizeof(response)) ||
        response.status != 0) {
        descriptor_close(static_cast<uint32_t>(reply_handle));
        return 1;
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
    surface.bytes_per_pixel = bytes_per_pixel;
    surface.format = response.format;

    uint32_t canvas_origin_x = kCanvasPadding;
    uint32_t canvas_origin_y = kToolbarHeight + kCanvasPadding;
    uint32_t palette_origin_y = (surface.height > kPaletteHeight + kCanvasPadding)
                                     ? surface.height - kPaletteHeight - kCanvasPadding
                                     : surface.height - kPaletteHeight;
    if (palette_origin_y <= canvas_origin_y + kCanvasPadding) {
        palette_origin_y = canvas_origin_y + kCanvasPadding + 1;
    }

    uint32_t canvas_width = (surface.width > canvas_origin_x * 2)
                                ? surface.width - canvas_origin_x * 2
                                : 1;
    uint32_t canvas_height = (palette_origin_y >
                              canvas_origin_y + kCanvasPadding)
                                 ? palette_origin_y - canvas_origin_y - kCanvasPadding
                                 : 1;

    uint32_t palette_item_y = palette_origin_y + kPalettePadding;
    uint32_t palette_gap = kPalettePadding;
    uint32_t palette_total_width =
        kPaletteCount * kPaletteSquare + (kPaletteCount - 1) * palette_gap;
    uint32_t palette_start_x = (surface.width > palette_total_width)
                                    ? (surface.width - palette_total_width) / 2
                                    : 0;

    uint32_t bg_color = lattice::pack_color(surface.format, 21, 22, 30);
    uint32_t toolbar_color = lattice::pack_color(surface.format, 34, 40, 52);
    uint32_t toolbar_border = lattice::pack_color(surface.format, 14, 16, 22);
    uint32_t canvas_bg = lattice::pack_color(surface.format, 255, 255, 255);
    uint32_t canvas_border = lattice::pack_color(surface.format, 80, 88, 108);
    uint32_t palette_bg = lattice::pack_color(surface.format, 18, 20, 26);
    uint32_t palette_border = lattice::pack_color(surface.format, 90, 98, 118);
    uint32_t text_color = lattice::pack_color(surface.format, 232, 235, 244);
    uint32_t ui_muted = lattice::pack_color(surface.format, 180, 185, 195);

    uint32_t palette_colors[kPaletteCount]{};
    for (size_t i = 0; i < kPaletteCount; ++i) {
        palette_colors[i] = lattice::pack_color(surface.format,
                                                kPaletteRGB[i].r,
                                                kPaletteRGB[i].g,
                                                kPaletteRGB[i].b);
    }

    static uint32_t canvas_pixels[kMaxCanvasPixels];

    auto clear_canvas = [&]() {
        size_t total = static_cast<size_t>(canvas_width) * canvas_height;
        for (size_t i = 0; i < total; ++i) {
            canvas_pixels[i] = canvas_bg;
        }
    };
    clear_canvas();

    size_t selected_color = 0;
    uint32_t brush_size = kDefaultBrush;
    bool needs_redraw = true;
    bool dirty_rect_valid = false;
    wm::ClientPresentRect dirty_rect{};

    auto color_from_index = [&]() -> uint32_t {
        return palette_colors[selected_color];
    };

    auto mark_dirty = [&](uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
        if (width == 0 || height == 0) {
            return;
        }
        if (!dirty_rect_valid) {
            dirty_rect = {};
            dirty_rect.x = static_cast<uint16_t>(x);
            dirty_rect.y = static_cast<uint16_t>(y);
            dirty_rect.width = static_cast<uint16_t>(width);
            dirty_rect.height = static_cast<uint16_t>(height);
            dirty_rect_valid = true;
            return;
        }
        uint32_t left = (dirty_rect.x < x) ? dirty_rect.x : x;
        uint32_t top = (dirty_rect.y < y) ? dirty_rect.y : y;
        uint32_t right0 = static_cast<uint32_t>(dirty_rect.x) + dirty_rect.width;
        uint32_t right1 = x + width;
        uint32_t bottom0 = static_cast<uint32_t>(dirty_rect.y) + dirty_rect.height;
        uint32_t bottom1 = y + height;
        uint32_t right = (right0 > right1) ? right0 : right1;
        uint32_t bottom = (bottom0 > bottom1) ? bottom0 : bottom1;
        dirty_rect.x = static_cast<uint16_t>(left);
        dirty_rect.y = static_cast<uint16_t>(top);
        dirty_rect.width = static_cast<uint16_t>(right - left);
        dirty_rect.height = static_cast<uint16_t>(bottom - top);
    };

    auto repaint_canvas_rect = [&](uint32_t rel_x,
                                   uint32_t rel_y,
                                   uint32_t width,
                                   uint32_t height) {
        if (width == 0 || height == 0) {
            return;
        }
        for (uint32_t row = 0; row < height; ++row) {
            uint32_t py = rel_y + row;
            if (py >= canvas_height) {
                break;
            }
            for (uint32_t col = 0; col < width; ++col) {
                uint32_t px = rel_x + col;
                if (px >= canvas_width) {
                    break;
                }
                uint32_t color = canvas_pixels[static_cast<size_t>(py) *
                                               canvas_width +
                                               static_cast<size_t>(px)];
                lattice::write_pixel(surface.buffer,
                                     surface.stride,
                                     surface.bytes_per_pixel,
                                     canvas_origin_x + px,
                                     canvas_origin_y + py,
                                     color);
            }
        }
    };

    auto paint_brush = [&](int32_t x, int32_t y) {
        if (canvas_width == 0 || canvas_height == 0) {
            return;
        }
        int32_t rel_x = x - static_cast<int32_t>(canvas_origin_x);
        int32_t rel_y = y - static_cast<int32_t>(canvas_origin_y);
        if (rel_x < 0 || rel_y < 0 ||
            rel_x >= static_cast<int32_t>(canvas_width) ||
            rel_y >= static_cast<int32_t>(canvas_height)) {
            return;
        }
        int32_t radius = static_cast<int32_t>(brush_size);
        int32_t min_x = rel_x - radius;
        int32_t min_y = rel_y - radius;
        int32_t max_x = rel_x + radius;
        int32_t max_y = rel_y + radius;
        if (min_x < 0) min_x = 0;
        if (min_y < 0) min_y = 0;
        if (max_x >= static_cast<int32_t>(canvas_width)) {
            max_x = static_cast<int32_t>(canvas_width) - 1;
        }
        if (max_y >= static_cast<int32_t>(canvas_height)) {
            max_y = static_cast<int32_t>(canvas_height) - 1;
        }
        for (int32_t dy = -radius; dy <= radius; ++dy) {
            int32_t py = rel_y + dy;
            if (py < 0 || py >= static_cast<int32_t>(canvas_height)) {
                continue;
            }
            for (int32_t dx = -radius; dx <= radius; ++dx) {
                int32_t px = rel_x + dx;
                if (px < 0 || px >= static_cast<int32_t>(canvas_width)) {
                    continue;
                }
                canvas_pixels[static_cast<size_t>(py) * canvas_width +
                              static_cast<size_t>(px)] = color_from_index();
            }
        }
        uint32_t dirty_x = static_cast<uint32_t>(canvas_origin_x + min_x);
        uint32_t dirty_y = static_cast<uint32_t>(canvas_origin_y + min_y);
        uint32_t dirty_w = static_cast<uint32_t>(max_x - min_x + 1);
        uint32_t dirty_h = static_cast<uint32_t>(max_y - min_y + 1);
        repaint_canvas_rect(static_cast<uint32_t>(min_x),
                            static_cast<uint32_t>(min_y),
                            dirty_w,
                            dirty_h);
        mark_dirty(dirty_x, dirty_y, dirty_w, dirty_h);
    };

    auto render_scene = [&]() {
        fill_rect(surface, 0, 0, surface.width, surface.height, bg_color);
        fill_rect(surface, 0, 0, surface.width, kToolbarHeight, toolbar_color);
        fill_rect(surface,
                  0,
                  kToolbarHeight - 1,
                  surface.width,
                  1,
                  toolbar_border);
        draw_text(surface,
                  canvas_origin_x,
                  6,
                  kProgramTitle,
                  text_color);
        draw_text(surface,
                  canvas_origin_x + 80,
                  6,
                  "Flux - paint anything",
                  ui_muted);
        const char instructions[] = "Left drag paints. File>Save/Load. C=clear. +/- = brush.";
        draw_text(surface,
                  canvas_origin_x,
                  kToolbarHeight - static_cast<int32_t>(g_font_size) - 1,
                  instructions,
                  ui_muted);

        fill_rect(surface,
                  canvas_origin_x - 2,
                  canvas_origin_y - 2,
                  canvas_width + 4,
                  canvas_height + 4,
                  canvas_border);
        fill_rect(surface,
                  canvas_origin_x,
                  canvas_origin_y,
                  canvas_width,
                  canvas_height,
                  canvas_bg);
        for (uint32_t row = 0; row < canvas_height; ++row) {
            for (uint32_t col = 0; col < canvas_width; ++col) {
                uint32_t color = canvas_pixels[static_cast<size_t>(row) *
                                               canvas_width +
                                               static_cast<size_t>(col)];
                lattice::write_pixel(surface.buffer,
                                     surface.stride,
                                     surface.bytes_per_pixel,
                                     canvas_origin_x + col,
                                     canvas_origin_y + row,
                                     color);
            }
        }

        fill_rect(surface,
                  0,
                  palette_origin_y - 4,
                  surface.width,
                  kPaletteHeight + 8,
                  palette_bg);
        fill_rect(surface,
                  0,
                  palette_origin_y - 4,
                  surface.width,
                  1,
                  palette_border);
        fill_rect(surface,
                  0,
                  palette_origin_y + kPaletteHeight + 3,
                  surface.width,
                  1,
                  palette_border);

        uint32_t palette_label_y = palette_origin_y -
                                   static_cast<uint32_t>(g_font_size) - 5;
        draw_text(surface,
                  canvas_origin_x,
                  static_cast<int32_t>(palette_label_y),
                  "Palette",
                  text_color);

        for (size_t i = 0; i < kPaletteCount; ++i) {
            uint32_t entry_x = palette_start_x +
                               static_cast<uint32_t>(i) * (kPaletteSquare + palette_gap);
            fill_rect(surface,
                      static_cast<int32_t>(entry_x),
                      static_cast<int32_t>(palette_item_y),
                      kPaletteSquare,
                      kPaletteSquare,
                      palette_colors[i]);
            uint32_t border_color =
                (i == selected_color) ? text_color : palette_border;
            fill_rect(surface,
                      static_cast<int32_t>(entry_x),
                      static_cast<int32_t>(palette_item_y),
                      kPaletteSquare,
                      1,
                      border_color);
            fill_rect(surface,
                      static_cast<int32_t>(entry_x),
                      static_cast<int32_t>(palette_item_y + kPaletteSquare - 1),
                      kPaletteSquare,
                      1,
                      border_color);
            fill_rect(surface,
                      static_cast<int32_t>(entry_x),
                      static_cast<int32_t>(palette_item_y),
                      1,
                      kPaletteSquare,
                      border_color);
            fill_rect(surface,
                      static_cast<int32_t>(entry_x + kPaletteSquare - 1),
                      static_cast<int32_t>(palette_item_y),
                      1,
                      kPaletteSquare,
                      border_color);
        }

        char brush_info[32];
        size_t brush_offset = 0;
        brush_offset = append_string(brush_info, sizeof(brush_info), brush_offset, "Brush: ");
        brush_offset = append_uint32(brush_info, sizeof(brush_info), brush_offset, brush_size);
        append_string(brush_info, sizeof(brush_info), brush_offset, " px");
        uint32_t brush_text_x = surface.width - g_font_size * 10;
        if (surface.width > brush_text_x + g_font_size * 4) {
            draw_text(surface,
                      static_cast<int32_t>(brush_text_x),
                      static_cast<int32_t>(palette_item_y + kPaletteSquare + 2),
                      brush_info,
                      text_color);
        }
    };

    if (response.out_pipe_id != 0) {
        uint64_t present_flags = static_cast<uint64_t>(descriptor_defs::Flag::Writable) |
                                 static_cast<uint64_t>(descriptor_defs::Flag::Async);
        long handle = pipe_open_existing(present_flags, response.out_pipe_id);
        if (handle >= 0) {
            uint32_t present_handle = static_cast<uint32_t>(handle);
            bool running = true;

            wm::MenuBar menu_bar{};
            init_menu_bar(menu_bar);

            lattice::FilePickerParent picker_parent{};
            picker_parent.buffer = surface.buffer;
            picker_parent.width = surface.width;
            picker_parent.height = surface.height;
            picker_parent.stride = surface.stride;
            picker_parent.bytes_per_pixel = surface.bytes_per_pixel;
            picker_parent.format = surface.format;
            picker_parent.reply_handle = static_cast<uint32_t>(reply_handle);
            picker_parent.present_handle = present_handle;

            auto save_via_picker = [&]() {
                lattice::FilePickerResult result =
                    lattice::FilePicker::open(picker_parent, lattice::FilePickerMode::Save);
                if (!result.accepted || result.handle == kInvalidDescriptor) {
                    return;
                }
                save_canvas_bmp_handle(result.handle,
                                       surface.format,
                                       canvas_pixels,
                                       canvas_width,
                                       canvas_height);
                file_close(result.handle);
            };

            auto load_via_picker = [&]() {
                lattice::FilePickerResult result =
                    lattice::FilePicker::open(picker_parent, lattice::FilePickerMode::Open);
                if (!result.accepted || result.handle == kInvalidDescriptor) {
                    return false;
                }
                bool ok = load_canvas_bmp_handle(result.handle,
                                                 surface.format,
                                                 canvas_pixels,
                                                 canvas_width,
                                                 canvas_height,
                                                 canvas_bg);
                file_close(result.handle);
                if (ok) {
                    needs_redraw = true;
                }
                return ok;
            };

            render_scene();
            if (present_handle != kInvalidDescriptor) {
                send_present(present_handle);
                send_menu_update(present_handle, menu_bar);
            }

            uint8_t buffer[2048];
            size_t pending = 0;

            while (running) {
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
                        running = false;
                        break;
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
                        bool new_left = (msg.buttons & 0x1u) != 0;
                        if (new_left) {
                            bool handled = false;
                            if (msg.y >= palette_item_y &&
                                msg.y < palette_item_y + kPaletteSquare) {
                                for (size_t i = 0; i < kPaletteCount; ++i) {
                                    uint32_t entry_x = palette_start_x +
                                                       static_cast<uint32_t>(i) * (kPaletteSquare + palette_gap);
                                    if (point_in_rect(msg.x,
                                                      msg.y,
                                                      entry_x,
                                                      palette_item_y,
                                                      kPaletteSquare,
                                                      kPaletteSquare)) {
                                        selected_color = i;
                                        needs_redraw = true;
                                        handled = true;
                                        break;
                                    }
                                }
                            }
                            if (!handled) {
                                paint_brush(static_cast<int32_t>(msg.x),
                                            static_cast<int32_t>(msg.y));
                                needs_redraw = true;
                            }
                        }
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
                        if (msg.id == kMenuIdSave) {
                            save_via_picker();
                        } else if (msg.id == kMenuIdLoad) {
                            load_via_picker();
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
                        offset += sizeof(msg);
                        descriptor_defs::KeyboardEvent event{};
                        event.scancode = msg.scancode;
                        event.flags = msg.flags;
                        event.mods = msg.mods;
                        event.reserved = 0;
                        if (!keyboard::is_pressed(event)) {
                            continue;
                        }
                        if (event.scancode == 0x01) {
                            running = false;
                            break;
                        }
                        char ch = keyboard::scancode_to_char(msg.scancode, msg.mods);
                        if (ch != 0) {
                            char lower = ch;
                            if (lower >= 'A' && lower <= 'Z') {
                                lower = static_cast<char>(lower - 'A' + 'a');
                            }
                            if (lower == 'c') {
                                clear_canvas();
                                needs_redraw = true;
                                continue;
                            }
                            if (ch == '+' || ch == '=') {
                                if (brush_size < kBrushMax) {
                                    ++brush_size;
                                    needs_redraw = true;
                                }
                                continue;
                            }
                            if (ch == '-') {
                                if (brush_size > kBrushMin) {
                                    --brush_size;
                                    needs_redraw = true;
                                }
                                continue;
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

                if (needs_redraw) {
                    render_scene();
                    if (present_handle != kInvalidDescriptor) {
                        if (dirty_rect_valid) {
                            send_present_rect(present_handle,
                                              dirty_rect.x,
                                              dirty_rect.y,
                                              dirty_rect.width,
                                              dirty_rect.height);
                        } else {
                            send_present(present_handle);
                        }
                    }
                    needs_redraw = false;
                    dirty_rect_valid = false;
                }

                yield();
            }
    descriptor_close(static_cast<uint32_t>(present_handle));
}
    }

    descriptor_close(static_cast<uint32_t>(reply_handle));
    descriptor_close(static_cast<uint32_t>(shm_handle));
    return 0;
}
