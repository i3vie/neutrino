#pragma once

#include <stddef.h>
#include <stdint.h>

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
    BlockReadFn read;
    BlockWriteFn write;
    void* context;
};

inline BlockIoStatus block_read(const BlockDevice& device, uint32_t lba,
                                uint8_t sector_count, void* buffer) {
    if (device.read == nullptr) {
        return BlockIoStatus::NoDevice;
    }
    return device.read(device.context, lba, sector_count, buffer);
}

inline BlockIoStatus block_write(const BlockDevice& device, uint32_t lba,
                                 uint8_t sector_count, const void* buffer) {
    if (device.write == nullptr) {
        return BlockIoStatus::NoDevice;
    }
    return device.write(device.context, lba, sector_count, buffer);
}

}  // namespace fs
