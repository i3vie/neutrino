#include "vfs.hpp"

#include <stddef.h>
#include <stdint.h>
#include "drivers/log/logging.hpp"
#include "lib/mem.hpp"

namespace vfs {

namespace {

size_t string_length(const char* str) {
    size_t len = 0;
    if (str == nullptr) {
        return 0;
    }
    while (str[len] != '\0') {
        ++len;
    }
    return len;
}

int string_compare_n(const char* a, const char* b, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        unsigned char ca = static_cast<unsigned char>(a[i]);
        unsigned char cb = static_cast<unsigned char>(b[i]);
        if (ca != cb) {
            return (ca < cb) ? -1 : 1;
        }
    }
    return 0;
}

bool string_contains_char(const char* str, char ch) {
    if (str == nullptr) {
        return false;
    }
    while (*str) {
        if (*str == ch) {
            return true;
        }
        ++str;
    }
    return false;
}

struct MountEntry {
    const char* name;
    Fat32Volume* volume;
    bool in_use;
};

constexpr size_t kMaxMounts = 16;
MountEntry g_mounts[kMaxMounts]{};

const char* skip_leading_slash(const char* path) {
    if (path == nullptr) return nullptr;
    while (*path == '/') ++path;
    return path;
}

MountEntry* find_mount(const char* name, size_t name_len) {
    for (auto& mount : g_mounts) {
        if (!mount.in_use) continue;
        if (string_length(mount.name) != name_len) continue;
        if (string_compare_n(mount.name, name, name_len) == 0) {
            return &mount;
        }
    }
    return nullptr;
}

MountEntry* find_mount_for_path(const char* path, const char*& remainder) {
    if (path == nullptr) return nullptr;

    const char* trimmed = skip_leading_slash(path);
    if (trimmed == nullptr || *trimmed == '\0') {
        remainder = trimmed;
        return nullptr;
    }

    const char* mount_end = trimmed;
    while (*mount_end != '\0' && *mount_end != '/') {
        ++mount_end;
    }

    size_t mount_len = static_cast<size_t>(mount_end - trimmed);
    MountEntry* entry = find_mount(trimmed, mount_len);
    if (entry == nullptr) {
        return nullptr;
    }

    const char* next = mount_end;
    while (*next == '/') ++next;
    remainder = next;
    return entry;
}

void fill_mount_entry(const MountEntry& mount, Fat32DirEntry& out) {
    memset(&out, 0, sizeof(Fat32DirEntry));
    size_t len = string_length(mount.name);
    if (len >= sizeof(out.name)) {
        len = sizeof(out.name) - 1;
    }
    memcpy(out.name, mount.name, len);
    out.attributes = 0x10;  // directory
}

}  // namespace

void init() {
    for (auto& mount : g_mounts) {
        mount = {};
    }
}

bool register_mount(const char* name, Fat32Volume* volume) {
    if (name == nullptr || *name == '\0' || volume == nullptr ||
        !volume->mounted) {
        log_message(LogLevel::Warn,
                    "VFS: register mount failed (invalid parameters)");
        return false;
    }

    size_t name_len = string_length(name);
    if (find_mount(name, name_len) != nullptr) {
        log_message(LogLevel::Warn, "VFS: mount '%s' already exists", name);
        return false;
    }

    for (auto& mount : g_mounts) {
        if (mount.in_use) continue;
        mount.name = name;
        mount.volume = volume;
        mount.in_use = true;
        log_message(LogLevel::Info, "VFS: mounted '%s'", name);
        return true;
    }

    log_message(LogLevel::Warn, "VFS: no free mount slots for '%s'", name);
    return false;
}

size_t enumerate_mounts(const char** names, size_t max_names) {
    size_t count = 0;
    for (auto& mount : g_mounts) {
        if (!mount.in_use) {
            continue;
        }
        if (count < max_names && names != nullptr) {
            names[count] = mount.name;
        }
        ++count;
    }
    return count;
}

bool list(const char* path, Fat32DirEntry* entries, size_t max_entries,
          size_t& out_count) {
    out_count = 0;
    if (entries == nullptr || max_entries == 0) {
        return false;
    }

    const char* remainder = nullptr;
    MountEntry* mount = find_mount_for_path(path, remainder);

    if (mount == nullptr) {
        log_message(LogLevel::Warn, "VFS: mount '%s' not found for list operation",
                    (path != nullptr) ? path : "(null)");
        return false;
    }

    if (remainder == nullptr || *remainder == '\0') {
        out_count = fat32_list_root(*mount->volume, entries, max_entries);
        return true;
    }

    log_message(LogLevel::Warn,
                "VFS: nested directories not supported (path=%s)", path);
    return false;
}

bool read_file(const char* path, void* buffer, size_t buffer_size,
               size_t& out_size) {
    out_size = 0;

    if (path == nullptr || buffer == nullptr) {
        return false;
    }

    const char* remainder = nullptr;
    MountEntry* mount = find_mount_for_path(path, remainder);
    if (mount == nullptr) {
        log_message(LogLevel::Warn, "VFS: mount not found for path '%s'", path);
        return false;
    }

    if (remainder == nullptr || *remainder == '\0') {
        log_message(LogLevel::Warn, "VFS: path '%s' refers to mount root", path);
        return false;
    }

    const char* trimmed = skip_leading_slash(remainder);
    if (trimmed == nullptr || *trimmed == '\0') {
        log_message(LogLevel::Warn, "VFS: path '%s' refers to mount root",
                    path);
        return false;
    }

    if (string_contains_char(trimmed, '/')) {
        log_message(LogLevel::Warn,
                    "VFS: nested directories not supported (path=%s)", path);
        return false;
    }

    return fat32_read_file(*mount->volume, trimmed, buffer, buffer_size,
                           out_size);
}

}  // namespace vfs
