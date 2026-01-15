#pragma once

#include <stdint.h>

#include "process.hpp"

namespace file_io {

int32_t open_file(process::Process& proc, const char* path);
int32_t create_file(process::Process& proc, const char* path);
int32_t open_file_at(process::Process& proc,
                     uint32_t dir_handle,
                     const char* name);
int32_t create_file_at(process::Process& proc,
                       uint32_t dir_handle,
                       const char* name);
bool close_file(process::Process& proc, uint32_t handle);
int64_t read_file(process::Process& proc, uint32_t handle, uint64_t user_addr,
                  uint64_t length);
int64_t write_file(process::Process& proc, uint32_t handle, uint64_t user_addr,
                   uint64_t length);

int32_t open_directory(process::Process& proc, const char* path);
int32_t open_directory_root(process::Process& proc);
int32_t open_directory_at(process::Process& proc,
                          uint32_t dir_handle,
                          const char* name);
bool close_directory(process::Process& proc, uint32_t handle);
int64_t read_directory(process::Process& proc, uint32_t handle,
                       uint64_t user_addr);

}  // namespace file_io
