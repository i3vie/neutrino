#include "settings.hpp"

#include <stddef.h>
#include <stdint.h>

#include "config.hpp"
#include "drivers/console/console.hpp"
#include "drivers/fs/block_cache.hpp"
#include "drivers/log/logging.hpp"
#include "fs/vfs.hpp"
#include "lib/mem.hpp"
#include "string_util.hpp"

extern "C" const unsigned char kernel_tosh_sat_f14[3584];

namespace settings {
namespace {

constexpr const char* kSettingsPathSuffix = "system/settings.ntd";
constexpr const char* kConsoleFontScaleKey = "CONSOLE.FONT_SCALE";
constexpr const char* kConsoleFontNameKey = "CONSOLE.FONT_NAME";
constexpr const char* kFontBasic = "basic";
constexpr const char* kFontToshiba = "toshiba";
constexpr size_t kMaxSettings = 32;
constexpr size_t kSettingsBufferSize = 4096;

struct Setting {
    char key[config::kMaxKeyLength];
    char value[config::kMaxValueLength];
};

char g_root_mount[64];
char g_settings_path[128];
Setting g_settings[kMaxSettings];
size_t g_setting_count = 0;
volatile int g_lock = 0;

void lock() {
    while (__atomic_test_and_set(&g_lock, __ATOMIC_ACQUIRE)) {
        asm volatile("pause");
    }
}

void unlock() {
    __atomic_clear(&g_lock, __ATOMIC_RELEASE);
}

class LockGuard {
public:
    LockGuard() { lock(); }
    ~LockGuard() { unlock(); }
};

bool path_ready() {
    return g_settings_path[0] != '\0';
}

bool append_text(char* out, size_t out_size, size_t& index, const char* text) {
    if (out == nullptr || text == nullptr) {
        return false;
    }
    for (size_t i = 0; text[i] != '\0'; ++i) {
        if (index + 1 >= out_size) {
            return false;
        }
        out[index++] = text[i];
    }
    out[index] = '\0';
    return true;
}

bool append_u32(char* out, size_t out_size, size_t& index, uint32_t value) {
    char digits[10];
    size_t count = 0;
    do {
        digits[count++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    } while (value != 0 && count < sizeof(digits));
    while (count != 0) {
        --count;
        if (index + 1 >= out_size) {
            return false;
        }
        out[index++] = digits[count];
    }
    out[index] = '\0';
    return true;
}

bool parse_u32(const char* text, uint32_t& out) {
    if (text == nullptr || *text == '\0') {
        return false;
    }
    uint32_t value = 0;
    const char* ptr = text;
    while (*ptr >= '0' && *ptr <= '9') {
        uint32_t digit = static_cast<uint32_t>(*ptr - '0');
        if (value > (UINT32_MAX - digit) / 10u) {
            return false;
        }
        value = value * 10u + digit;
        ++ptr;
    }
    if (*ptr != '\0') {
        return false;
    }
    out = value;
    return true;
}

Setting* find_setting(const char* key) {
    if (key == nullptr) {
        return nullptr;
    }
    for (size_t i = 0; i < g_setting_count; ++i) {
        if (string_util::equals(g_settings[i].key, key)) {
            return &g_settings[i];
        }
    }
    return nullptr;
}

const Setting* find_setting_const(const char* key) {
    if (key == nullptr) {
        return nullptr;
    }
    for (size_t i = 0; i < g_setting_count; ++i) {
        if (string_util::equals(g_settings[i].key, key)) {
            return &g_settings[i];
        }
    }
    return nullptr;
}

bool set_string_locked(const char* key, const char* value) {
    if (key == nullptr || value == nullptr || key[0] == '\0') {
        return false;
    }
    if (string_util::length(key) >= config::kMaxKeyLength ||
        string_util::length(value) >= config::kMaxValueLength) {
        return false;
    }

    Setting* setting = find_setting(key);
    if (setting == nullptr) {
        if (g_setting_count >= kMaxSettings) {
            return false;
        }
        setting = &g_settings[g_setting_count++];
        string_util::copy(setting->key, sizeof(setting->key), key);
    }
    string_util::copy(setting->value, sizeof(setting->value), value);
    return true;
}

bool build_path() {
    if (g_root_mount[0] == '\0') {
        g_settings_path[0] = '\0';
        return false;
    }
    size_t index = 0;
    g_settings_path[0] = '\0';
    if (!append_text(g_settings_path, sizeof(g_settings_path), index, g_root_mount) ||
        !append_text(g_settings_path, sizeof(g_settings_path), index, "/") ||
        !append_text(g_settings_path,
                     sizeof(g_settings_path),
                     index,
                     kSettingsPathSuffix)) {
        g_settings_path[0] = '\0';
        return false;
    }
    return true;
}

void ensure_directory(const char* path) {
    vfs::DirectoryHandle handle{};
    if (vfs::open_directory(path, handle)) {
        vfs::close_directory(handle);
        return;
    }
    if (!vfs::create_directory(path)) {
        log_message(LogLevel::Warn,
                    "Settings: failed to create directory '%s'",
                    path);
    }
}

void ensure_parent_directory() {
    if (!path_ready()) {
        return;
    }
    char parent[128];
    string_util::copy(parent, sizeof(parent), g_root_mount);
    size_t len = string_util::length(parent);
    if (len + 8 >= sizeof(parent)) {
        return;
    }
    parent[len++] = '/';
    parent[len] = '\0';
    string_util::copy(parent + len, sizeof(parent) - len, "system");
    ensure_directory(parent);
}

bool serialize(char* out, size_t out_size, size_t& out_len) {
    out_len = 0;
    out[0] = '\0';
    if (!append_text(out, out_size, out_len, "# Neutrino kernel settings\n")) {
        return false;
    }
    for (size_t i = 0; i < g_setting_count; ++i) {
        if (!append_text(out, out_size, out_len, g_settings[i].key) ||
            !append_text(out, out_size, out_len, ": ") ||
            !append_text(out, out_size, out_len, g_settings[i].value) ||
            !append_text(out, out_size, out_len, "\n")) {
            return false;
        }
    }
    return true;
}

bool font_is_basic(const descriptor_defs::ConsoleFont& font,
                   const uint8_t* data) {
    return data != nullptr && font.width == 8 && font.height == 8 &&
           font.glyph_count == 128 && font.bytes_per_row == 1 &&
           font.data_size == sizeof(font8x8_basic) && font.flags == 0 &&
           memcmp(data, &font8x8_basic[0][0], sizeof(font8x8_basic)) == 0;
}

bool font_is_toshiba(const descriptor_defs::ConsoleFont& font,
                     const uint8_t* data) {
    constexpr size_t kDataSize = sizeof(kernel_tosh_sat_f14);
    return data != nullptr && font.width == 8 && font.height == 14 &&
           font.glyph_count == 256 && font.bytes_per_row == 1 &&
           font.data_size == kDataSize &&
           font.flags == descriptor_defs::kConsoleFontMsbFirst &&
           memcmp(data, kernel_tosh_sat_f14, kDataSize) == 0;
}

bool set_builtin_font(Console& console, const char* name) {
    if (string_util::equals(name, kFontBasic)) {
        descriptor_defs::ConsoleFont font{
            8,
            8,
            128,
            1,
            sizeof(font8x8_basic),
            0,
        };
        return console.set_font(font, &font8x8_basic[0][0]);
    }
    if (string_util::equals(name, kFontToshiba)) {
        descriptor_defs::ConsoleFont font{
            8,
            14,
            256,
            1,
            sizeof(kernel_tosh_sat_f14),
            descriptor_defs::kConsoleFontMsbFirst,
        };
        return console.set_font(font, kernel_tosh_sat_f14);
    }
    return false;
}

}  // namespace

void set_storage_root(const char* root_mount_name) {
    LockGuard guard;
    string_util::copy(g_root_mount, sizeof(g_root_mount), root_mount_name);
    build_path();
}

bool load_from_disk() {
    LockGuard guard;
    g_setting_count = 0;
    if (!path_ready()) {
        return false;
    }

    uint8_t buffer[kSettingsBufferSize];
    size_t read_size = 0;
    if (!vfs::read_file(g_settings_path, buffer, sizeof(buffer), read_size)) {
        return false;
    }

    config::Table table{};
    bool parse_ok =
        config::parse(reinterpret_cast<const char*>(buffer), read_size, table);
    for (size_t i = 0; i < table.count; ++i) {
        set_string_locked(table.entries[i].key, table.entries[i].value);
    }
    if (!parse_ok) {
        log_message(LogLevel::Warn,
                    "Settings: %s parsed with errors",
                    g_settings_path);
    } else {
        log_message(LogLevel::Info,
                    "Settings: loaded %u entr%s from %s",
                    static_cast<unsigned int>(g_setting_count),
                    g_setting_count == 1 ? "y" : "ies",
                    g_settings_path);
    }
    return parse_ok;
}

bool save_to_disk() {
    LockGuard guard;
    if (!path_ready()) {
        return false;
    }

    char buffer[kSettingsBufferSize];
    size_t length = 0;
    if (!serialize(buffer, sizeof(buffer), length)) {
        log_message(LogLevel::Warn, "Settings: serialized data too large");
        return false;
    }

    ensure_parent_directory();
    (void)vfs::remove_file(g_settings_path);

    vfs::FileHandle handle{};
    if (!vfs::create_file(g_settings_path, handle)) {
        log_message(LogLevel::Warn,
                    "Settings: failed to create %s",
                    g_settings_path);
        return false;
    }

    size_t written = 0;
    bool ok = vfs::write_file(handle, 0, buffer, length, written);
    vfs::close_file(handle);
    if (!ok || written != length) {
        log_message(LogLevel::Warn,
                    "Settings: failed to write %s",
                    g_settings_path);
        return false;
    }
    (void)fs::block_cache::flush_all();
    return true;
}

const char* get_string(const char* key) {
    LockGuard guard;
    const Setting* setting = find_setting_const(key);
    return setting != nullptr ? setting->value : nullptr;
}

bool get_u32(const char* key, uint32_t& out) {
    LockGuard guard;
    const Setting* setting = find_setting_const(key);
    return setting != nullptr && parse_u32(setting->value, out);
}

bool set_string(const char* key, const char* value) {
    LockGuard guard;
    return set_string_locked(key, value);
}

bool set_u32(const char* key, uint32_t value) {
    char buffer[16];
    size_t index = 0;
    if (!append_u32(buffer, sizeof(buffer), index, value)) {
        return false;
    }
    LockGuard guard;
    return set_string_locked(key, buffer);
}

bool apply_console_preferences(Console& console) {
    bool changed = false;

    const char* font_name = get_string(kConsoleFontNameKey);
    if (font_name != nullptr) {
        if (set_builtin_font(console, font_name)) {
            changed = true;
        } else {
            log_message(LogLevel::Warn,
                        "Settings: unknown console font '%s'",
                        font_name);
        }
    }

    uint32_t scale = 0;
    if (get_u32(kConsoleFontScaleKey, scale)) {
        if (console.set_scale(scale)) {
            changed = true;
        } else {
            log_message(LogLevel::Warn,
                        "Settings: invalid console scale %u",
                        static_cast<unsigned int>(scale));
        }
    }

    if (changed) {
        console.clear();
    }
    return changed;
}

void persist_console_scale(uint32_t scale) {
    if (set_u32(kConsoleFontScaleKey, scale)) {
        (void)save_to_disk();
    }
}

void persist_console_font(const descriptor_defs::ConsoleFont& font,
                          const uint8_t* data) {
    const char* name = nullptr;
    if (font_is_basic(font, data)) {
        name = kFontBasic;
    } else if (font_is_toshiba(font, data)) {
        name = kFontToshiba;
    }

    if (name == nullptr) {
        log_message(LogLevel::Debug,
                    "Settings: console font is not a known builtin; not persisted");
        return;
    }

    if (set_string(kConsoleFontNameKey, name)) {
        (void)save_to_disk();
    }
}

}  // namespace settings
