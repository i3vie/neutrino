#pragma once

#include <stdint.h>

#include "process.hpp"

namespace file_io {

int32_t open_file(process::Process& proc, const char* path);
int32_t create_file(process::Process& proc, const char* path);
bool close_file(process::Process& proc, uint32_t handle);
int64_t read_file(process::Process& proc, uint32_t handle, uint64_t user_addr,
                  uint64_t length);
int64_t write_file(process::Process& proc, uint32_t handle, uint64_t user_addr,
                   uint64_t length);

int32_t open_directory(process::Process& proc, const char* path);
bool close_directory(process::Process& proc, uint32_t handle);
int64_t read_directory(process::Process& proc, uint32_t handle,
                       uint64_t user_addr);

}  // namespace file_io
