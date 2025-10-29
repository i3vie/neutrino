#include "file_io.hpp"

#include <limits.h>
#include <stddef.h>

#include "drivers/log/logging.hpp"
#include "fs/vfs.hpp"
#include "lib/mem.hpp"
#include "string_util.hpp"

namespace {

constexpr size_t kMaxPathLength = 128;

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
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

bool copy_path(const char* user_path, char (&out)[kMaxPathLength]) {
    if (user_path == nullptr) {
        return false;
    }
    string_util::copy(out, sizeof(out), user_path);
    if (out[0] == '\0') {
        return false;
    }
    return true;
}

}  // namespace

namespace file_io {

int32_t open_file(process::Process& proc, const char* path) {
    char local_path[kMaxPathLength];
    if (!copy_path(path, local_path)) {
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

int64_t write_file(process::Process& proc, uint32_t handle, uint64_t,
                   uint64_t) {
    (void)proc;
    (void)handle;
    log_message(LogLevel::Warn,
                "FileIO: write not supported on current filesystem backend");
    return -1;
}

int32_t open_directory(process::Process& proc, const char* path) {
    char local_path[kMaxPathLength];
    if (!copy_path(path, local_path)) {
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
