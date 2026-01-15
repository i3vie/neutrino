#include "file_io.hpp"

#include <limits.h>
#include <stddef.h>

#include "drivers/log/logging.hpp"
#include "fs/vfs.hpp"
#include "lib/mem.hpp"
#include "path_util.hpp"
#include "string_util.hpp"

namespace {

process::FileHandle* get_file_handle(process::Process& proc, uint32_t handle) {
    if (handle >= process::kMaxFileHandles) {
        return nullptr;
    }
    process::FileHandle& entry = proc.file_handles[handle];
    return entry.in_use ? &entry : nullptr;
}

process::DirectoryHandle* get_directory_handle(process::Process& proc,
                                               uint32_t handle) {
    if (handle >= process::kMaxDirectoryHandles) {
        return nullptr;
    }
    process::DirectoryHandle& entry = proc.directory_handles[handle];
    return entry.in_use ? &entry : nullptr;
}

int32_t allocate_file_handle(process::Process& proc) {
    for (uint32_t i = 0; i < process::kMaxFileHandles; ++i) {
        if (!proc.file_handles[i].in_use) {
            proc.file_handles[i].in_use = true;
            proc.file_handles[i].handle = {};
            proc.file_handles[i].position = 0;
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

int32_t allocate_directory_handle(process::Process& proc) {
    for (uint32_t i = 0; i < process::kMaxDirectoryHandles; ++i) {
        if (!proc.directory_handles[i].in_use) {
            proc.directory_handles[i].in_use = true;
            proc.directory_handles[i].handle = {};
            proc.directory_handles[i].path[0] = '\0';
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

bool copy_path(process::Process& proc,
               const char* user_path,
               char (&out)[path_util::kMaxPathLength]) {
    if (user_path == nullptr) {
        return false;
    }

    size_t input_len = string_util::length(user_path);
    if (input_len == 0 || input_len >= path_util::kMaxPathLength) {
        return false;
    }

    char temp[path_util::kMaxPathLength];
    string_util::copy(temp, sizeof(temp), user_path);
    return path_util::build_absolute_path(proc.cwd, temp, out);
}

bool build_child_path(const char* base,
                      const char* name,
                      char (&out)[path_util::kMaxPathLength]) {
    if (base == nullptr || name == nullptr) {
        return false;
    }
    if (name[0] == '\0') {
        return false;
    }
    if (string_util::contains(name, '/')) {
        return false;
    }
    return path_util::build_absolute_path(base, name, out);
}

}  // namespace

namespace file_io {

int32_t open_file(process::Process& proc, const char* path) {
    char local_path[path_util::kMaxPathLength];
    if (!copy_path(proc, path, local_path)) {
        return -1;
    }

    int32_t slot = allocate_file_handle(proc);
    if (slot < 0) {
        log_message(LogLevel::Warn,
                    "FileIO: no free file handles for process %u",
                    static_cast<unsigned int>(proc.pid));
        return -1;
    }

    vfs::FileHandle vfs_handle{};
    if (!vfs::open_file(local_path, vfs_handle)) {
        proc.file_handles[static_cast<size_t>(slot)].in_use = false;
        proc.file_handles[static_cast<size_t>(slot)].handle = {};
        return -1;
    }

    process::FileHandle& handle = proc.file_handles[static_cast<size_t>(slot)];
    handle.handle = vfs_handle;
    handle.position = 0;
    return slot;
}

int32_t create_file(process::Process& proc, const char* path) {
    char local_path[path_util::kMaxPathLength];
    if (!copy_path(proc, path, local_path)) {
        return -1;
    }

    int32_t slot = allocate_file_handle(proc);
    if (slot < 0) {
        log_message(LogLevel::Warn,
                    "FileIO: no free file handles for process %u",
                    static_cast<unsigned int>(proc.pid));
        return -1;
    }

    vfs::FileHandle vfs_handle{};
    if (!vfs::create_file(local_path, vfs_handle)) {
        proc.file_handles[static_cast<size_t>(slot)].in_use = false;
        proc.file_handles[static_cast<size_t>(slot)].handle = {};
        return -1;
    }

    process::FileHandle& handle = proc.file_handles[static_cast<size_t>(slot)];
    handle.handle = vfs_handle;
    handle.position = 0;
    return slot;
}

bool close_file(process::Process& proc, uint32_t handle) {
    process::FileHandle* entry = get_file_handle(proc, handle);
    if (entry == nullptr) {
        return false;
    }
    vfs::close_file(entry->handle);
    entry->in_use = false;
    entry->handle = {};
    entry->position = 0;
    return true;
}

int64_t read_file(process::Process& proc, uint32_t handle, uint64_t user_addr,
                  uint64_t length) {
    process::FileHandle* entry = get_file_handle(proc, handle);
    if (entry == nullptr) {
        return -1;
    }
    if (length == 0) {
        return 0;
    }
    size_t requested =
        (length > static_cast<uint64_t>(SIZE_MAX))
            ? static_cast<size_t>(SIZE_MAX)
            : static_cast<size_t>(length);
    void* buffer = reinterpret_cast<void*>(user_addr);
    size_t out_size = 0;
    if (!vfs::read_file(entry->handle,
                        entry->position,
                        buffer,
                        requested,
                        out_size)) {
        return -1;
    }
    entry->position += static_cast<uint64_t>(out_size);
    return static_cast<int64_t>(out_size);
}

int64_t write_file(process::Process& proc, uint32_t handle, uint64_t user_addr,
                   uint64_t length) {
    process::FileHandle* entry = get_file_handle(proc, handle);
    if (entry == nullptr) {
        return -1;
    }
    if (length == 0) {
        return 0;
    }

    size_t requested =
        (length > static_cast<uint64_t>(SIZE_MAX))
            ? static_cast<size_t>(SIZE_MAX)
            : static_cast<size_t>(length);
    const void* buffer = reinterpret_cast<const void*>(user_addr);
    size_t out_size = 0;
    if (!vfs::write_file(entry->handle,
                         entry->position,
                         buffer,
                         requested,
                         out_size)) {
        return -1;
    }

    entry->position += static_cast<uint64_t>(out_size);
    return static_cast<int64_t>(out_size);
}

int32_t open_directory(process::Process& proc, const char* path) {
    char local_path[path_util::kMaxPathLength];
    if (!copy_path(proc, path, local_path)) {
        return -1;
    }

    vfs::DirectoryHandle vfs_handle{};
    if (!vfs::open_directory(local_path, vfs_handle)) {
        return -1;
    }

    int32_t slot = allocate_directory_handle(proc);
    if (slot < 0) {
        log_message(LogLevel::Warn,
                    "FileIO: no free directory handles for process %u",
                    static_cast<unsigned int>(proc.pid));
        vfs::close_directory(vfs_handle);
        return -1;
    }

    process::DirectoryHandle& handle =
        proc.directory_handles[static_cast<size_t>(slot)];
    handle.handle = vfs_handle;
    string_util::copy(handle.path, sizeof(handle.path), local_path);
    return slot;
}

int32_t open_directory_root(process::Process& proc) {
    char local_path[path_util::kMaxPathLength];
    local_path[0] = '/';
    local_path[1] = '\0';

    vfs::DirectoryHandle vfs_handle{};
    if (!vfs::open_directory(local_path, vfs_handle)) {
        return -1;
    }

    int32_t slot = allocate_directory_handle(proc);
    if (slot < 0) {
        log_message(LogLevel::Warn,
                    "FileIO: no free directory handles for process %u",
                    static_cast<unsigned int>(proc.pid));
        vfs::close_directory(vfs_handle);
        return -1;
    }

    process::DirectoryHandle& handle =
        proc.directory_handles[static_cast<size_t>(slot)];
    handle.handle = vfs_handle;
    string_util::copy(handle.path, sizeof(handle.path), local_path);
    return slot;
}

int32_t open_directory_at(process::Process& proc,
                          uint32_t dir_handle,
                          const char* name) {
    process::DirectoryHandle* parent = get_directory_handle(proc, dir_handle);
    if (parent == nullptr) {
        return -1;
    }
    char local_path[path_util::kMaxPathLength];
    if (!build_child_path(parent->path, name, local_path)) {
        return -1;
    }

    vfs::DirectoryHandle vfs_handle{};
    if (!vfs::open_directory(local_path, vfs_handle)) {
        return -1;
    }

    int32_t slot = allocate_directory_handle(proc);
    if (slot < 0) {
        log_message(LogLevel::Warn,
                    "FileIO: no free directory handles for process %u",
                    static_cast<unsigned int>(proc.pid));
        vfs::close_directory(vfs_handle);
        return -1;
    }

    process::DirectoryHandle& handle =
        proc.directory_handles[static_cast<size_t>(slot)];
    handle.handle = vfs_handle;
    string_util::copy(handle.path, sizeof(handle.path), local_path);
    return slot;
}

int32_t open_file_at(process::Process& proc,
                     uint32_t dir_handle,
                     const char* name) {
    process::DirectoryHandle* parent = get_directory_handle(proc, dir_handle);
    if (parent == nullptr) {
        return -1;
    }
    char local_path[path_util::kMaxPathLength];
    if (!build_child_path(parent->path, name, local_path)) {
        return -1;
    }

    int32_t slot = allocate_file_handle(proc);
    if (slot < 0) {
        log_message(LogLevel::Warn,
                    "FileIO: no free file handles for process %u",
                    static_cast<unsigned int>(proc.pid));
        return -1;
    }

    vfs::FileHandle vfs_handle{};
    if (!vfs::open_file(local_path, vfs_handle)) {
        proc.file_handles[static_cast<size_t>(slot)].in_use = false;
        proc.file_handles[static_cast<size_t>(slot)].handle = {};
        return -1;
    }

    process::FileHandle& handle = proc.file_handles[static_cast<size_t>(slot)];
    handle.handle = vfs_handle;
    handle.position = 0;
    return slot;
}

int32_t create_file_at(process::Process& proc,
                       uint32_t dir_handle,
                       const char* name) {
    process::DirectoryHandle* parent = get_directory_handle(proc, dir_handle);
    if (parent == nullptr) {
        return -1;
    }
    char local_path[path_util::kMaxPathLength];
    if (!build_child_path(parent->path, name, local_path)) {
        return -1;
    }

    int32_t slot = allocate_file_handle(proc);
    if (slot < 0) {
        log_message(LogLevel::Warn,
                    "FileIO: no free file handles for process %u",
                    static_cast<unsigned int>(proc.pid));
        return -1;
    }

    vfs::FileHandle vfs_handle{};
    if (!vfs::create_file(local_path, vfs_handle)) {
        proc.file_handles[static_cast<size_t>(slot)].in_use = false;
        proc.file_handles[static_cast<size_t>(slot)].handle = {};
        return -1;
    }

    process::FileHandle& handle = proc.file_handles[static_cast<size_t>(slot)];
    handle.handle = vfs_handle;
    handle.position = 0;
    return slot;
}

bool close_directory(process::Process& proc, uint32_t handle) {
    process::DirectoryHandle* entry = get_directory_handle(proc, handle);
    if (entry == nullptr) {
        return false;
    }
    vfs::close_directory(entry->handle);
    entry->in_use = false;
    entry->handle = {};
    entry->path[0] = '\0';
    return true;
}

int64_t read_directory(process::Process& proc, uint32_t handle,
                       uint64_t user_addr) {
    process::DirectoryHandle* entry = get_directory_handle(proc, handle);
    if (entry == nullptr) {
        return -1;
    }

    vfs::DirEntry result{};
    if (!vfs::read_directory(entry->handle, result)) {
        return 0;
    }

    auto* dest = reinterpret_cast<vfs::DirEntry*>(user_addr);
    memcpy(dest, &result, sizeof(vfs::DirEntry));
    return 1;
}

}  // namespace file_io
