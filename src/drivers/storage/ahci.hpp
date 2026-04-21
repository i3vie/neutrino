#pragma once

#include <stddef.h>
#include <stdint.h>

namespace ahci {

enum class Status {
    Ok,
    Busy,
    NoDevice,
    IoError,
    Unsupported,
};

struct IdentifyInfo {
    bool present;
    char model[41];
    uint64_t sector_count;
};

bool init();

size_t device_count();
const IdentifyInfo& identify(size_t device_index);
const char* device_name(size_t device_index);

Status read_sectors(size_t device_index, uint64_t lba, uint8_t sector_count,
                    void* buffer);
Status write_sectors(size_t device_index, uint64_t lba, uint8_t sector_count,
                     const void* buffer);

}  // namespace ahci
