#include "file_io.hpp"

#include <limits.h>
#include <stddef.h>

#include "drivers/log/logging.hpp"
#include "drivers/fs/block_cache.hpp"
#include "fs/vfs.hpp"
#include "lib/mem.hpp"
#include "capabilities.hpp"
#include "path_util.hpp"
#include "string_util.hpp"
#include "vm.hpp"

namespace {

// Match the largest filesystem cluster so sequential writes reach the VFS as
// full-cluster operations instead of repeated partial-cluster updates.
constexpr size_t kFileIoBounceSize = 32768;
alignas(4096) uint8_t g_file_io_bounce[kFileIoBounceSize];
volatile int g_file_io_bounce_lock = 0;

void lock_file_io_bounce() {
    while (__atomic_test_and_set(&g_file_io_bounce_lock, __ATOMIC_ACQUIRE)) {
        asm volatile("pause");
    }
}

void unlock_file_io_bounce() {
    __atomic_clear(&g_file_io_bounce_lock, __ATOMIC_RELEASE);
}

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
            proc.file_handles[i].can_write = false;
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

    char temp[path_util::kMaxPathLength];
    if (!vm::copy_user_string(proc.cr3,
                              user_path,
                              temp,
                              sizeof(temp)) ||
        temp[0] == '\0') {
        return false;
    }
    return path_util::build_absolute_path(proc.cwd, temp, out);
}

bool build_child_path(process::Process& proc,
                      const char* base,
                      const char* name,
                      char (&out)[path_util::kMaxPathLength]) {
    if (base == nullptr || name == nullptr) {
        return false;
    }
    char local_name[path_util::kMaxPathLength];
    if (!vm::copy_user_string(proc.cr3,
                              name,
                              local_name,
                              sizeof(local_name)) ||
        local_name[0] == '\0') {
        return false;
    }
    if (string_util::contains(local_name, '/')) {
        return false;
    }
    return path_util::build_absolute_path(base, local_name, out);
}

vfs::AclValue permission_value(const vfs::AclEntry& entry,
                               vfs::AclPermission permission) {
    switch (permission) {
        case vfs::AclPermission::Read: return entry.read;
        case vfs::AclPermission::Write: return entry.write;
        case vfs::AclPermission::Delete: return entry.delete_permission;
        case vfs::AclPermission::Edit: return entry.edit;
    }
    return vfs::AclValue::Deny;
}

bool acl_allows(const process::Process& proc,
                const char* path,
                vfs::AclPermission permission) {
    if (proc.principal == nullptr ||
        capabilities::principal_allows(
            *proc.principal,
            capabilities::CapabilityKind::SecurityManage)) {
        return true;
    }
    if (!vfs::acl_supported(path)) {
        return true;
    }

    vfs::AclEntry entries[vfs::kMaxAclEntries]{};
    size_t count = 0;
    if (!vfs::get_acl(path, entries, vfs::kMaxAclEntries, count)) {
        return false;
    }
    if (count == 0) {
        return true;
    }

    uint64_t machine_id = 0;
    uint64_t local_id = 0;
    if (!capabilities::principal_user_id(proc.principal,
                                         machine_id,
                                         local_id)) {
        return false;
    }

    for (size_t i = 0; i < count; ++i) {
        const vfs::AclEntry& entry = entries[i];
        if (entry.machine_id != machine_id || entry.local_id != local_id) {
            continue;
        }
        vfs::AclValue value = permission_value(entry, permission);
        return value == vfs::AclValue::Allow;
    }
    return false;
}

bool acl_check_required(const process::Process& proc) {
    return proc.principal != nullptr &&
           !capabilities::principal_allows(
               *proc.principal,
               capabilities::CapabilityKind::SecurityManage);
}

struct AclDecision {
    bool read;
    bool write;
    bool delete_permission;
    bool edit;
};

AclDecision acl_snapshot_decision(const process::Process& proc,
                                  const vfs::AclSnapshot& acl) {
    if (!acl.supported || acl.count == 0) {
        return {true, true, true, true};
    }
    uint64_t machine_id = 0;
    uint64_t local_id = 0;
    if (!capabilities::principal_user_id(proc.principal,
                                         machine_id,
                                         local_id)) {
        return {};
    }
    for (size_t i = 0; i < acl.count; ++i) {
        const vfs::AclEntry& entry = acl.entries[i];
        if (entry.machine_id == machine_id && entry.local_id == local_id) {
            return {
                entry.read == vfs::AclValue::Allow,
                entry.write == vfs::AclValue::Allow,
                entry.delete_permission == vfs::AclValue::Allow,
                entry.edit == vfs::AclValue::Allow,
            };
        }
    }
    return {};
}

bool parent_path(const char* path,
                 char (&out)[path_util::kMaxPathLength]) {
    size_t length = string_util::length(path);
    while (length > 1 && path[length - 1] == '/') {
        --length;
    }
    size_t slash = length;
    while (slash > 0 && path[slash - 1] != '/') {
        --slash;
    }
    size_t parent_length = slash > 1 ? slash - 1 : 1;
    if (parent_length >= sizeof(out)) {
        return false;
    }
    memcpy(out, path, parent_length);
    out[parent_length] = '\0';
    return true;
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
    vfs::AclSnapshot acl{};
    bool check_acl = acl_check_required(proc);
    if (!vfs::open_file(local_path,
                        vfs_handle,
                        check_acl ? &acl : nullptr)) {
        vfs::close_file(vfs_handle);
        proc.file_handles[static_cast<size_t>(slot)].in_use = false;
        proc.file_handles[static_cast<size_t>(slot)].handle = {};
        return -1;
    }
    AclDecision decision = check_acl
                               ? acl_snapshot_decision(proc, acl)
                               : AclDecision{true, true, true, true};
    if (!decision.read) {
        vfs::close_file(vfs_handle);
        proc.file_handles[static_cast<size_t>(slot)].in_use = false;
        proc.file_handles[static_cast<size_t>(slot)].handle = {};
        return -1;
    }

    process::FileHandle& handle = proc.file_handles[static_cast<size_t>(slot)];
    handle.handle = vfs_handle;
    handle.can_write = decision.write;
    handle.position = 0;
    return slot;
}

int32_t create_file(process::Process& proc, const char* path) {
    char local_path[path_util::kMaxPathLength];
    if (!copy_path(proc, path, local_path)) {
        return -1;
    }
    char parent[path_util::kMaxPathLength];
    if (!parent_path(local_path, parent) ||
        !acl_allows(proc, parent, vfs::AclPermission::Write)) {
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
    handle.can_write = true;
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
    entry->can_write = false;
    entry->handle = {};
    entry->position = 0;
    return true;
}

bool sync_file(process::Process& proc, uint32_t handle) {
    process::FileHandle* entry = get_file_handle(proc, handle);
    if (entry == nullptr) {
        return false;
    }
    return fs::block_cache::flush_all();
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
    if (user_addr == 0) {
        return -1;
    }

    size_t total_read = 0;
    while (total_read < requested) {
        size_t chunk = requested - total_read;
        if (chunk > kFileIoBounceSize) {
            chunk = kFileIoBounceSize;
        }

        size_t out_size = 0;
        lock_file_io_bounce();
        if (!vfs::read_file(entry->handle,
                            entry->position + total_read,
                            g_file_io_bounce,
                            chunk,
                            out_size)) {
            unlock_file_io_bounce();
            return total_read == 0 ? -1 : static_cast<int64_t>(total_read);
        }
        if (out_size == 0) {
            unlock_file_io_bounce();
            break;
        }
        if (!vm::copy_to_user(proc.cr3,
                              user_addr + total_read,
                              g_file_io_bounce,
                              out_size)) {
            unlock_file_io_bounce();
            return total_read == 0 ? -1 : static_cast<int64_t>(total_read);
        }
        unlock_file_io_bounce();
        total_read += out_size;
        if (out_size < chunk) {
            break;
        }
    }

    entry->position += static_cast<uint64_t>(total_read);
    return static_cast<int64_t>(total_read);
}

int64_t write_file(process::Process& proc, uint32_t handle, uint64_t user_addr,
                   uint64_t length) {
    process::FileHandle* entry = get_file_handle(proc, handle);
    if (entry == nullptr) {
        return -1;
    }
    if (!entry->can_write) {
        return -1;
    }
    if (length == 0) {
        return 0;
    }

    size_t requested =
        (length > static_cast<uint64_t>(SIZE_MAX))
            ? static_cast<size_t>(SIZE_MAX)
            : static_cast<size_t>(length);
    if (user_addr == 0) {
        return -1;
    }

    size_t total_written = 0;
    while (total_written < requested) {
        size_t chunk = requested - total_written;
        if (chunk > kFileIoBounceSize) {
            chunk = kFileIoBounceSize;
        }

        lock_file_io_bounce();
        if (!vm::copy_from_user(proc.cr3,
                                g_file_io_bounce,
                                user_addr + total_written,
                                chunk)) {
            unlock_file_io_bounce();
            return total_written == 0 ? -1 : static_cast<int64_t>(total_written);
        }

        size_t out_size = 0;
        if (!vfs::write_file(entry->handle,
                             entry->position + total_written,
                             g_file_io_bounce,
                             chunk,
                             out_size)) {
            unlock_file_io_bounce();
            return total_written == 0 ? -1 : static_cast<int64_t>(total_written);
        }
        if (out_size == 0) {
            unlock_file_io_bounce();
            break;
        }
        unlock_file_io_bounce();
        total_written += out_size;
        if (out_size < chunk) {
            break;
        }
    }

    entry->position += static_cast<uint64_t>(total_written);
    return static_cast<int64_t>(total_written);
}

int32_t open_directory(process::Process& proc, const char* path) {
    char local_path[path_util::kMaxPathLength];
    if (!copy_path(proc, path, local_path)) {
        return -1;
    }
    vfs::DirectoryHandle vfs_handle{};
    vfs::AclSnapshot acl{};
    bool check_acl = acl_check_required(proc);
    if (!vfs::open_directory(local_path,
                             vfs_handle,
                             check_acl ? &acl : nullptr)) {
        vfs::close_directory(vfs_handle);
        return -1;
    }
    if (check_acl && !acl_snapshot_decision(proc, acl).read) {
        vfs::close_directory(vfs_handle);
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
    if (!build_child_path(proc, parent->path, name, local_path)) {
        return -1;
    }
    vfs::DirectoryHandle vfs_handle{};
    vfs::AclSnapshot acl{};
    bool check_acl = acl_check_required(proc);
    if (!vfs::open_directory(local_path,
                             vfs_handle,
                             check_acl ? &acl : nullptr)) {
        vfs::close_directory(vfs_handle);
        return -1;
    }
    if (check_acl && !acl_snapshot_decision(proc, acl).read) {
        vfs::close_directory(vfs_handle);
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

bool create_directory(process::Process& proc, const char* path) {
    char local_path[path_util::kMaxPathLength];
    if (!copy_path(proc, path, local_path)) {
        return false;
    }
    char parent[path_util::kMaxPathLength];
    return parent_path(local_path, parent) &&
           acl_allows(proc, parent, vfs::AclPermission::Write) &&
           vfs::create_directory(local_path);
}

bool remove_file(process::Process& proc, const char* path) {
    char local_path[path_util::kMaxPathLength];
    if (!copy_path(proc, path, local_path)) {
        return false;
    }
    if (!acl_allows(proc, local_path, vfs::AclPermission::Delete)) {
        return false;
    }
    return vfs::remove_file(local_path);
}

bool remove_directory(process::Process& proc, const char* path) {
    char local_path[path_util::kMaxPathLength];
    if (!copy_path(proc, path, local_path)) {
        return false;
    }
    return acl_allows(proc, local_path, vfs::AclPermission::Delete) &&
           vfs::remove_directory(local_path);
}

int32_t open_file_at(process::Process& proc,
                     uint32_t dir_handle,
                     const char* name) {
    process::DirectoryHandle* parent = get_directory_handle(proc, dir_handle);
    if (parent == nullptr) {
        return -1;
    }
    char local_path[path_util::kMaxPathLength];
    if (!build_child_path(proc, parent->path, name, local_path)) {
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
    vfs::AclSnapshot acl{};
    bool check_acl = acl_check_required(proc);
    if (!vfs::open_file(local_path,
                        vfs_handle,
                        check_acl ? &acl : nullptr)) {
        vfs::close_file(vfs_handle);
        proc.file_handles[static_cast<size_t>(slot)].in_use = false;
        proc.file_handles[static_cast<size_t>(slot)].handle = {};
        return -1;
    }
    AclDecision decision = check_acl
                               ? acl_snapshot_decision(proc, acl)
                               : AclDecision{true, true, true, true};
    if (!decision.read) {
        vfs::close_file(vfs_handle);
        proc.file_handles[static_cast<size_t>(slot)].in_use = false;
        proc.file_handles[static_cast<size_t>(slot)].handle = {};
        return -1;
    }

    process::FileHandle& handle = proc.file_handles[static_cast<size_t>(slot)];
    handle.handle = vfs_handle;
    handle.can_write = decision.write;
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
    if (!build_child_path(proc, parent->path, name, local_path)) {
        return -1;
    }
    char parent_path_buffer[path_util::kMaxPathLength];
    if (!parent_path(local_path, parent_path_buffer) ||
        !acl_allows(proc,
                    parent_path_buffer,
                    vfs::AclPermission::Write)) {
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
    handle.can_write = true;
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

    if (!vm::copy_to_user(proc.cr3, user_addr, &result, sizeof(vfs::DirEntry))) {
        return -1;
    }
    return 1;
}

int64_t get_acl(process::Process& proc,
                const char* path,
                uint64_t user_entries,
                uint64_t max_entries) {
    if (max_entries == 0 || max_entries > vfs::kMaxAclEntries ||
        user_entries == 0) {
        return -1;
    }
    char local_path[path_util::kMaxPathLength];
    if (!copy_path(proc, path, local_path)) {
        return -1;
    }
    vfs::AclEntry entries[vfs::kMaxAclEntries]{};
    size_t count = 0;
    if (!vfs::get_acl(local_path,
                      entries,
                      static_cast<size_t>(max_entries),
                      count) ||
        !vm::copy_to_user(proc.cr3,
                          user_entries,
                          entries,
                          count * sizeof(vfs::AclEntry))) {
        return -1;
    }
    return static_cast<int64_t>(count);
}

bool set_acl(process::Process& proc,
             const char* path,
             uint64_t user_entries,
             uint64_t entry_count) {
    if (entry_count > vfs::kMaxAclEntries ||
        (entry_count != 0 && user_entries == 0)) {
        return false;
    }
    char local_path[path_util::kMaxPathLength];
    if (!copy_path(proc, path, local_path)) {
        return false;
    }
    vfs::AclEntry entries[vfs::kMaxAclEntries]{};
    if (entry_count != 0 &&
        !vm::copy_from_user(proc.cr3,
                            entries,
                            user_entries,
                            static_cast<size_t>(entry_count) *
                                sizeof(vfs::AclEntry))) {
        return false;
    }
    return vfs::set_acl(local_path,
                        entries,
                        static_cast<size_t>(entry_count));
}

}  // namespace file_io
