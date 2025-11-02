#pragma once

#include <stddef.h>
#include <stdint.h>

namespace vfs {

enum : uint32_t {
    kDirEntryFlagDirectory = 1u << 0,
};

struct DirEntry {
    char name[64];
    uint32_t flags;
    uint32_t reserved;
    uint64_t size;
};

struct FilesystemOps;

struct FileHandle {
    const FilesystemOps* ops;
    void* fs_context;
    void* file_context;
    uint64_t size;
};

struct DirectoryHandle {
    const FilesystemOps* ops;
    void* fs_context;
    void* dir_context;
    bool is_root;
};

struct FilesystemOps {
    bool (*list_directory)(void* fs_context,
                           const char* path,
                           DirEntry* entries,
                           size_t max_entries,
                           size_t& out_count);
    bool (*open_file)(void* fs_context,
                      const char* path,
                      void*& out_file_context,
                      DirEntry* out_metadata);
    bool (*create_file)(void* fs_context,
                        const char* path,
                        void*& out_file_context,
                        DirEntry* out_metadata);
    bool (*read_file)(void* file_context,
                      uint64_t offset,
                      void* buffer,
                      size_t buffer_size,
                      size_t& out_size);
    bool (*write_file)(void* file_context,
                       uint64_t offset,
                       const void* buffer,
                       size_t buffer_size,
                       size_t& out_size);
    void (*close_file)(void* file_context);
    bool (*open_directory)(void* fs_context,
                           const char* path,
                           void*& out_dir_context);
    bool (*directory_next)(void* dir_context, DirEntry& out_entry);
    void (*close_directory)(void* dir_context);
};

void init();
bool register_mount(const char* name,
                    const FilesystemOps* ops,
                    void* fs_context);
size_t enumerate_mounts(const char** names, size_t max_names);

bool list(const char* path,
          DirEntry* entries,
          size_t max_entries,
          size_t& out_count);
bool read_file(const char* path,
               void* buffer,
               size_t buffer_size,
               size_t& out_size);

bool open_file(const char* path, FileHandle& out_handle);
bool create_file(const char* path, FileHandle& out_handle);
void close_file(FileHandle& handle);
bool read_file(FileHandle& handle,
               uint64_t offset,
               void* buffer,
               size_t buffer_size,
               size_t& out_size);
bool write_file(FileHandle& handle,
                uint64_t offset,
                const void* buffer,
                size_t buffer_size,
                size_t& out_size);

bool open_directory(const char* path, DirectoryHandle& out_handle);
bool read_directory(DirectoryHandle& handle, DirEntry& out_entry);
void close_directory(DirectoryHandle& handle);

}  // namespace vfs
