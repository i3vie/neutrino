#pragma once
#include <stddef.h>

#include "block_device.hpp"

namespace fs {

using BlockDeviceEnumerateFn =
    size_t (*)(BlockDevice* out_devices, size_t max_devices);

using FilesystemProbeFn = bool (*)(const BlockDevice& device);

void register_block_device_provider(BlockDeviceEnumerateFn fn);
void register_filesystem_driver(FilesystemProbeFn fn);

// mount only the devices explicitly requested. returns true if the root
// filesystem (when specified) was mounted successfully and writes the total
// number of filesystems mounted to out_total_mounted.
bool mount_requested_filesystems(const char* root_spec,
                                 const char* const* mount_specs,
                                 size_t mount_count,
                                 size_t& out_total_mounted);

}  // namespace fs
