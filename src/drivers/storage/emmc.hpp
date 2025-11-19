#pragma once

#include <stddef.h>
#include <stdint.h>

namespace emmc {

enum class Status {
    Ok,
    IoError,
    NoDevice,
    Busy,
};

bool init();

size_t device_count();

uint64_t device_sector_count(size_t index);

Status read_blocks(size_t index, uint32_t lba, uint8_t count, void* buffer);

}  // namespace emmc

