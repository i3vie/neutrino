#pragma once

#include <stddef.h>
#include <stdint.h>

#include "kernel/descriptor.hpp"

namespace fs {

enum class BlockIoStatus {
    Ok,
    IoError,
    NoDevice,
    Busy,
};

using BlockReadFn =
    BlockIoStatus (*)(void* context, uint32_t lba, uint8_t sector_count,
                      void* buffer);
using BlockWriteFn =
    BlockIoStatus (*)(void* context, uint32_t lba, uint8_t sector_count,
                      const void* buffer);

struct BlockDevice {
    const char* name;
    size_t sector_size;
    uint64_t sector_count;
    uint32_t descriptor_handle = descriptor::kInvalidHandle;
    BlockReadFn read;
    BlockWriteFn write;
    void* context;
};

inline BlockIoStatus block_read(const BlockDevice& device, uint32_t lba,
                                uint8_t sector_count, void* buffer) {
    if (device.descriptor_handle != descriptor::kInvalidHandle) {
        uint64_t length = static_cast<uint64_t>(sector_count) * device.sector_size;
        uint64_t offset = static_cast<uint64_t>(lba) * device.sector_size;
        int64_t result = descriptor::read_kernel(device.descriptor_handle,
                                                 buffer,
                                                 length,
                                                 offset);
        if (result == static_cast<int64_t>(length)) {
            return BlockIoStatus::Ok;
        }
        return BlockIoStatus::IoError;
    }
    if (device.read == nullptr) {
        return BlockIoStatus::NoDevice;
    }
    return device.read(device.context, lba, sector_count, buffer);
}

inline BlockIoStatus block_write(const BlockDevice& device, uint32_t lba,
                                 uint8_t sector_count, const void* buffer) {
    if (device.descriptor_handle != descriptor::kInvalidHandle) {
        uint64_t length = static_cast<uint64_t>(sector_count) * device.sector_size;
        uint64_t offset = static_cast<uint64_t>(lba) * device.sector_size;
        int64_t result = descriptor::write_kernel(device.descriptor_handle,
                                                  buffer,
                                                  length,
                                                  offset);
        if (result == static_cast<int64_t>(length)) {
            return BlockIoStatus::Ok;
        }
        return BlockIoStatus::IoError;
    }
    if (device.write == nullptr) {
        return BlockIoStatus::NoDevice;
    }
    return device.write(device.context, lba, sector_count, buffer);
}

}  // namespace fs
