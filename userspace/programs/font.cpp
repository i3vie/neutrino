#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <neutrino.h>

#include "descriptors.hpp"
#include "font8x8_basic.hpp"
#include "../crt/syscall.hpp"

extern "C" const unsigned char tosh_sat_f14[3584];

asm(
    ".section .rodata\n"
    ".balign 1\n"
    ".global tosh_sat_f14\n"
    ".hidden tosh_sat_f14\n"
    "tosh_sat_f14:\n"
    ".incbin \"../shared/include/TOSH-SAT.F14\"\n");

namespace {

const char* skip_spaces(const char* text) {
    while (text != nullptr && *text == ' ') {
        ++text;
    }
    return text;
}

bool token_is(const char* text, const char* expected) {
    text = skip_spaces(text);
    while (*expected != '\0' && *text == *expected) {
        ++text;
        ++expected;
    }
    return *expected == '\0' && (*text == '\0' || *text == ' ');
}

bool parse_scale(const char* text, uint32_t& out) {
    text = skip_spaces(text);
    if (text == nullptr || *text == '\0') {
        return false;
    }
    uint32_t value = 0;
    while (*text >= '0' && *text <= '9') {
        value = value * 10 + static_cast<uint32_t>(*text - '0');
        ++text;
    }
    text = skip_spaces(text);
    if (*text != '\0' || value < descriptor_defs::kConsoleMinScale ||
        value > descriptor_defs::kConsoleMaxScale) {
        return false;
    }
    out = value;
    return true;
}

void write_u32(long console, uint32_t value) {
    char digits[10];
    size_t count = 0;
    do {
        digits[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0);
    while (count != 0) {
        --count;
        descriptor_write(static_cast<uint32_t>(console), &digits[count], 1);
    }
}

template <size_t Height>
bool upload_font(long console,
                 const unsigned char (&glyphs)[128][Height],
                 uint16_t width,
                 uint32_t flags) {
    constexpr size_t kDataSize = sizeof(glyphs);
    struct Payload {
        descriptor_defs::ConsoleFont info;
        uint8_t data[kDataSize];
    } payload{};
    static_assert(offsetof(Payload, data) ==
                  sizeof(descriptor_defs::ConsoleFont));

    payload.info.width = width;
    payload.info.height = static_cast<uint16_t>(Height);
    payload.info.glyph_count = 128;
    payload.info.bytes_per_row = 1;
    payload.info.data_size = kDataSize;
    payload.info.flags = flags;
    memcpy(payload.data, glyphs, kDataSize);

    return console_set_font(static_cast<uint32_t>(console),
                            &payload,
                            sizeof(payload)) == 0;
}

bool upload_tosh_sat(long console) {
    constexpr size_t kDataSize = sizeof(tosh_sat_f14);
    struct Payload {
        descriptor_defs::ConsoleFont info;
        uint8_t data[kDataSize];
    } payload{};
    static_assert(kDataSize == 256 * 14);
    static_assert(offsetof(Payload, data) ==
                  sizeof(descriptor_defs::ConsoleFont));

    payload.info.width = 8;
    payload.info.height = 14;
    payload.info.glyph_count = 256;
    payload.info.bytes_per_row = 1;
    payload.info.data_size = kDataSize;
    payload.info.flags = descriptor_defs::kConsoleFontMsbFirst;
    memcpy(payload.data, tosh_sat_f14, kDataSize);

    return console_set_font(static_cast<uint32_t>(console),
                            &payload,
                            sizeof(payload)) == 0;
}

void print_info(long console) {
    descriptor_defs::ConsoleFont font{};
    uint32_t scale = 0;
    if (console_get_font(static_cast<uint32_t>(console),
                         &font,
                         sizeof(font)) != 0 ||
        console_get_scale(static_cast<uint32_t>(console), &scale) != 0) {
        neutrino_write_line(console, "font: unable to query console font");
        return;
    }

    neutrino_write(console, "font: ");
    write_u32(console, font.width);
    neutrino_write(console, "x");
    write_u32(console, font.height);
    neutrino_write(console, ", glyphs=");
    write_u32(console, font.glyph_count);
    neutrino_write(console, ", scale=");
    write_u32(console, scale);
    neutrino_write_line(console, "");
}

}  // namespace

int main(uint64_t arg_ptr, uint64_t) {
    const char* args = skip_spaces(reinterpret_cast<const char*>(arg_ptr));
    long console = neutrino_open_stdout();
    if (console < 0) {
        return 1;
    }

    if (args == nullptr || *args == '\0' || token_is(args, "info")) {
        print_info(console);
        return 0;
    }

    bool loaded = false;
    const char* name = nullptr;
    if (token_is(args, "basic")) {
        loaded = upload_font(console, font8x8_basic, 8, 0);
        name = "basic 8x8";
    } else if (token_is(args, "toshiba")) {
        loaded = upload_tosh_sat(console);
        name = "TOSH-SAT 8x14";
    } else if (token_is(args, "scale")) {
        const char* value = args + 5;
        uint32_t scale = 0;
        if (!parse_scale(value, scale)) {
            neutrino_write_line(console, "usage: font scale <1-8>");
            return 1;
        }
        if (console_set_scale(static_cast<uint32_t>(console), scale) != 0) {
            neutrino_write_line(console, "font: unable to set scale");
            return 1;
        }
        print_info(console);
        return 0;
    } else {
        neutrino_write_line(console,
                            "usage: font [info|basic|toshiba|scale <1-8>]");
        return 1;
    }

    if (!loaded) {
        neutrino_write_line(console, "font: upload failed");
        return 1;
    }
    neutrino_write(console, "font: loaded ");
    neutrino_write_line(console, name);
    neutrino_write_line(console,
                        "The quick brown fox jumps over the lazy dog. 0123456789");
    return 0;
}
