#pragma once
#include <stddef.h>
#include <stdint.h>

enum class IdeStatus {
    Ok,
    Busy,
    DmaUnsupported,
    NoDevice,
    IoError,
};

struct IdeIdentifyInfo {
    bool present;
    char model[41];
    uint32_t sector_count;
};

enum class IdeDeviceId : uint8_t {
    PrimaryMaster = 0,
    PrimarySlave,
    SecondaryMaster,
    SecondarySlave,
};

bool ide_init(IdeDeviceId device);
bool ide_init();

const IdeIdentifyInfo& ide_identify(IdeDeviceId device);
const IdeIdentifyInfo& ide_primary_identify();
const char* ide_device_name(IdeDeviceId device);

IdeStatus ide_read_sectors(IdeDeviceId device, uint32_t lba, uint8_t sector_count,
                           void* buffer);
IdeStatus ide_read_sectors(uint32_t lba, uint8_t sector_count, void* buffer);
IdeStatus ide_write_sectors(IdeDeviceId device, uint32_t lba,
                            uint8_t sector_count, const void* buffer);
IdeStatus ide_write_sectors(uint32_t lba, uint8_t sector_count,
                            const void* buffer);
