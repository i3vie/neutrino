#pragma once

#include "drivers/usb/usb_core.hpp"

namespace usb::mass_storage {

enum class Status {
    Ok,
    Busy,
    NoDevice,
    IoError,
    Unsupported,
    Timeout,
};

struct IdentifyInfo {
    bool present;
    char vendor[9];
    char product[17];
    uint64_t sector_count;
    uint32_t sector_size;
    bool removable;
};

void init();
bool probe_device(const usb::Device& device);

size_t device_count();
const IdentifyInfo& identify(size_t device_index);
const char* device_name(size_t device_index);

Status read_sectors(size_t device_index,
                    uint64_t lba,
                    uint8_t sector_count,
                    void* buffer);

Status write_sectors(size_t device_index,
                     uint64_t lba,
                     uint8_t sector_count,
                     const void* buffer);

}  // namespace usb::mass_storage

namespace fs {

void register_usb_mass_storage_block_device_provider();

}  // namespace fs
