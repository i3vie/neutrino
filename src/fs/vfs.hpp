#pragma once
#include <stddef.h>
#include <stdint.h>

#include "drivers/fs/fat32/fat32.hpp"

namespace vfs {

void init();
bool register_mount(const char* name, Fat32Volume* volume);
size_t enumerate_mounts(const char** names, size_t max_names);
bool list(const char* path, Fat32DirEntry* entries, size_t max_entries,
          size_t& out_count);
bool read_file(const char* path, void* buffer, size_t buffer_size,
               size_t& out_size);

}  // namespace vfs
