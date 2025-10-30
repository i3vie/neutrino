#include "vfs.hpp"

#include <stddef.h>
#include <stdint.h>

#include "drivers/log/logging.hpp"
#include "lib/mem.hpp"

namespace vfs {

namespace {

constexpr size_t kMaxMounts = 16;

struct MountEntry {
    const char* name;
    const FilesystemOps* ops;
    void* fs_context;
    bool in_use;
};

MountEntry g_mounts[kMaxMounts]{};

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

const char* skip_leading_slash(const char* path) {
    if (path == nullptr) {
        return nullptr;
    }
    while (*path == '/') {
        ++path;
    }
    return path;
}

MountEntry* find_mount(const char* name, size_t name_len) {
    for (auto& mount : g_mounts) {
        if (!mount.in_use) {
            continue;
        }
        if (string_length(mount.name) != name_len) {
            continue;
        }
        if (string_compare_n(mount.name, name, name_len) == 0) {
            return &mount;
        }
    }
    return nullptr;
}

MountEntry* find_mount_for_path(const char* path, const char*& remainder) {
    if (path == nullptr) {
        return nullptr;
    }

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
    while (*next == '/') {
        ++next;
    }
    remainder = next;
    return entry;
}

const char* normalize_relative_path(const char* path) {
    if (path == nullptr) {
        return "";
    }
    return path;
}

}  // namespace

void init() {
    for (auto& mount : g_mounts) {
        mount = {};
    }
}

bool register_mount(const char* name,
                    const FilesystemOps* ops,
                    void* fs_context) {
    if (name == nullptr || *name == '\0' || ops == nullptr) {
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
        if (mount.in_use) {
            continue;
        }
        mount.name = name;
        mount.ops = ops;
        mount.fs_context = fs_context;
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
        if (names != nullptr && count < max_names) {
            names[count] = mount.name;
        }
        ++count;
    }
    return count;
}

bool list(const char* path,
          DirEntry* entries,
          size_t max_entries,
          size_t& out_count) {
    out_count = 0;
    if (entries == nullptr || max_entries == 0) {
        return false;
    }

    const char* remainder = nullptr;
    MountEntry* mount = find_mount_for_path(path, remainder);
    if (mount == nullptr) {
        log_message(LogLevel::Warn,
                    "VFS: mount '%s' not found for list operation",
                    (path != nullptr) ? path : "(null)");
        return false;
    }

    if (mount->ops == nullptr || mount->ops->list_directory == nullptr) {
        return false;
    }

    const char* relative = normalize_relative_path(remainder);
    return mount->ops->list_directory(mount->fs_context,
                                      relative,
                                      entries,
                                      max_entries,
                                      out_count);
}

bool read_file(const char* path,
               void* buffer,
               size_t buffer_size,
               size_t& out_size) {
    out_size = 0;
    if (buffer == nullptr) {
        return false;
    }

    FileHandle handle{};
    if (!open_file(path, handle)) {
        return false;
    }

    bool ok = read_file(handle, 0, buffer, buffer_size, out_size);
    close_file(handle);
    return ok;
}

bool open_file(const char* path, FileHandle& out_handle) {
    out_handle = {};

    const char* remainder = nullptr;
    MountEntry* mount = find_mount_for_path(path, remainder);
    if (mount == nullptr) {
        log_message(LogLevel::Warn,
                    "VFS: mount not found for path '%s'",
                    (path != nullptr) ? path : "(null)");
        return false;
    }

    if (mount->ops == nullptr || mount->ops->open_file == nullptr ||
        mount->ops->read_file == nullptr || mount->ops->close_file == nullptr) {
        return false;
    }

    const char* relative = normalize_relative_path(remainder);
    if (relative == nullptr || *relative == '\0') {
        return false;
    }

    DirEntry metadata{};
    void* file_context = nullptr;
    if (!mount->ops->open_file(mount->fs_context,
                               relative,
                               file_context,
                               &metadata)) {
        return false;
    }

    out_handle.ops = mount->ops;
    out_handle.fs_context = mount->fs_context;
    out_handle.file_context = file_context;
    out_handle.size = metadata.size;
    return true;
}

bool create_file(const char* path, FileHandle& out_handle) {
    out_handle = {};

    const char* remainder = nullptr;
    MountEntry* mount = find_mount_for_path(path, remainder);
    if (mount == nullptr) {
        log_message(LogLevel::Warn,
                    "VFS: mount not found for path '%s'",
                    (path != nullptr) ? path : "(null)");
        return false;
    }

    if (mount->ops == nullptr || mount->ops->create_file == nullptr ||
        mount->ops->read_file == nullptr || mount->ops->write_file == nullptr ||
        mount->ops->close_file == nullptr) {
        return false;
    }

    const char* relative = normalize_relative_path(remainder);
    if (relative == nullptr || *relative == '\0') {
        return false;
    }

    DirEntry metadata{};
    void* file_context = nullptr;
    if (!mount->ops->create_file(mount->fs_context,
                                 relative,
                                 file_context,
                                 &metadata)) {
        return false;
    }

    out_handle.ops = mount->ops;
    out_handle.fs_context = mount->fs_context;
    out_handle.file_context = file_context;
    out_handle.size = metadata.size;
    return true;
}

void close_file(FileHandle& handle) {
    if (handle.ops != nullptr && handle.ops->close_file != nullptr &&
        handle.file_context != nullptr) {
        handle.ops->close_file(handle.file_context);
    }
    handle = {};
}

bool read_file(FileHandle& handle,
               uint64_t offset,
               void* buffer,
               size_t buffer_size,
               size_t& out_size) {
    out_size = 0;
    if (handle.ops == nullptr || handle.ops->read_file == nullptr ||
        handle.file_context == nullptr || buffer == nullptr) {
        return false;
    }
    return handle.ops->read_file(handle.file_context,
                                 offset,
                                 buffer,
                                 buffer_size,
                                 out_size);
}

bool write_file(FileHandle& handle,
                uint64_t offset,
                const void* buffer,
                size_t buffer_size,
                size_t& out_size) {
    out_size = 0;
    if (handle.ops == nullptr || handle.ops->write_file == nullptr ||
        handle.file_context == nullptr || buffer == nullptr) {
        return false;
    }
    if (!handle.ops->write_file(handle.file_context,
                                offset,
                                buffer,
                                buffer_size,
                                out_size)) {
        return false;
    }

    uint64_t end_offset = offset + static_cast<uint64_t>(out_size);
    if (end_offset > handle.size) {
        handle.size = end_offset;
    }
    return true;
}

bool open_directory(const char* path, DirectoryHandle& out_handle) {
    out_handle = {};

    const char* remainder = nullptr;
    MountEntry* mount = find_mount_for_path(path, remainder);
    if (mount == nullptr) {
        log_message(LogLevel::Warn,
                    "VFS: mount not found for path '%s'",
                    (path != nullptr) ? path : "(null)");
        return false;
    }

    if (mount->ops == nullptr || mount->ops->open_directory == nullptr ||
        mount->ops->directory_next == nullptr ||
        mount->ops->close_directory == nullptr) {
        return false;
    }

    const char* relative = normalize_relative_path(remainder);
    void* dir_context = nullptr;
    if (!mount->ops->open_directory(mount->fs_context,
                                    relative,
                                    dir_context)) {
        return false;
    }

    out_handle.ops = mount->ops;
    out_handle.fs_context = mount->fs_context;
    out_handle.dir_context = dir_context;
    return true;
}

bool read_directory(DirectoryHandle& handle, DirEntry& out_entry) {
    if (handle.ops == nullptr || handle.ops->directory_next == nullptr ||
        handle.dir_context == nullptr) {
        return false;
    }
    return handle.ops->directory_next(handle.dir_context, out_entry);
}

void close_directory(DirectoryHandle& handle) {
    if (handle.ops != nullptr && handle.ops->close_directory != nullptr &&
        handle.dir_context != nullptr) {
        handle.ops->close_directory(handle.dir_context);
    }
    handle = {};
}

}  // namespace vfs
