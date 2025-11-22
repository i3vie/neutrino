#include "ide.hpp"

#include <stddef.h>
#include <stdint.h>

#include "arch/x86_64/io.hpp"
#include "lib/mem.hpp"

namespace {

constexpr uint16_t ATA_REG_DATA = 0x00;
constexpr uint16_t ATA_REG_ERROR = 0x01;
constexpr uint16_t ATA_REG_SECCOUNT0 = 0x02;
constexpr uint16_t ATA_REG_LBA0 = 0x03;
constexpr uint16_t ATA_REG_LBA1 = 0x04;
constexpr uint16_t ATA_REG_LBA2 = 0x05;
constexpr uint16_t ATA_REG_HDDEVSEL = 0x06;
constexpr uint16_t ATA_REG_COMMAND = 0x07;
constexpr uint16_t ATA_REG_STATUS = 0x07;

constexpr uint8_t ATA_CMD_IDENTIFY = 0xEC;
constexpr uint8_t ATA_CMD_READ_SECTORS = 0x20;
constexpr uint8_t ATA_CMD_WRITE_SECTORS = 0x30;

constexpr uint8_t ATA_SR_BSY = 0x80;
constexpr uint8_t ATA_SR_DRDY = 0x40;
constexpr uint8_t ATA_SR_DRQ = 0x08;
constexpr uint8_t ATA_SR_ERR = 0x01;

struct IdeDeviceDescriptor {
    uint16_t io_base;
    uint16_t ctrl_base;
    uint8_t device_select;
};

constexpr IdeDeviceDescriptor kDeviceTable[] = {
    {0x1F0, 0x3F6, 0xA0},  // Primary master
    {0x1F0, 0x3F6, 0xB0},  // Primary slave
    {0x170, 0x376, 0xA0},  // Secondary master
    {0x170, 0x376, 0xB0},  // Secondary slave
};

constexpr const char* kDeviceNames[] = {
    "primary master",
    "primary slave",
    "secondary master",
    "secondary slave",
};

constexpr size_t kDeviceCount =
    sizeof(kDeviceTable) / sizeof(kDeviceTable[0]);

struct IdeDeviceState {
    bool probed;
    IdeIdentifyInfo identify;
};

IdeDeviceState g_device_state[kDeviceCount]{};

constexpr size_t device_index(IdeDeviceId device) {
    return static_cast<size_t>(device);
}

const IdeDeviceDescriptor& device_desc(IdeDeviceId device) {
    return kDeviceTable[device_index(device)];
}

IdeDeviceState& device_state(IdeDeviceId device) {
    return g_device_state[device_index(device)];
}

const char* device_name(IdeDeviceId device) {
    return kDeviceNames[device_index(device)];
}

uint8_t io_read8(const IdeDeviceDescriptor& desc, uint16_t reg) {
    return inb(desc.io_base + reg);
}

void io_write8(const IdeDeviceDescriptor& desc, uint16_t reg,
               uint8_t value) {
    outb(desc.io_base + reg, value);
}

void io_write_ctrl(const IdeDeviceDescriptor& desc, uint8_t value) {
    outb(desc.ctrl_base, value);
}

bool wait_not_busy(IdeDeviceId device, uint32_t timeout = 100000) {
    const auto& desc = device_desc(device);
    while (timeout--) {
        uint8_t status = io_read8(desc, ATA_REG_STATUS);
        if ((status & ATA_SR_BSY) == 0) {
            return true;
        }
    }
    return false;
}

bool wait_drq(IdeDeviceId device, uint32_t timeout = 100000) {
    const auto& desc = device_desc(device);
    while (timeout--) {
        uint8_t status = io_read8(desc, ATA_REG_STATUS);
        if (status & ATA_SR_ERR) {
            return false;
        }
        if (status & ATA_SR_DRQ) {
            return true;
        }
    }
    return false;
}

void read_data(IdeDeviceId device, uint16_t* buffer, size_t words) {
    const auto& desc = device_desc(device);
    for (size_t i = 0; i < words; ++i) {
        buffer[i] = inw(desc.io_base + ATA_REG_DATA);
    }
}

void write_data(IdeDeviceId device, const uint16_t* buffer, size_t words) {
    const auto& desc = device_desc(device);
    for (size_t i = 0; i < words; ++i) {
        outw(desc.io_base + ATA_REG_DATA, buffer[i]);
    }
}

void swap_bytes(char* dest, const uint16_t* src, size_t word_count) {
    for (size_t i = 0; i < word_count; ++i) {
        dest[i * 2 + 0] = static_cast<char>(src[i] >> 8);
        dest[i * 2 + 1] = static_cast<char>(src[i] & 0xFF);
    }
}

void trim_string(char* str, size_t len) {
    for (size_t i = len; i > 0; --i) {
        if (str[i - 1] == ' ' || str[i - 1] == '\0') {
            str[i - 1] = '\0';
        } else {
            break;
        }
    }
}

bool identify_device(IdeDeviceId device) {
    auto& state = device_state(device);
    if (state.probed) {
        return state.identify.present;
    }

    state.identify.present = false;
    memset(state.identify.model, 0, sizeof(state.identify.model));
    state.identify.sector_count = 0;

    const auto& desc = device_desc(device);

    uint8_t status = io_read8(desc, ATA_REG_STATUS);
    if (status == 0xFF) {
        state.probed = true;
        return false;
    }

    io_write_ctrl(desc, 0x02);  // disable interrupts

    io_write8(desc, ATA_REG_HDDEVSEL, desc.device_select);
    io_wait();

    io_write8(desc, ATA_REG_SECCOUNT0, 0);
    io_write8(desc, ATA_REG_LBA0, 0);
    io_write8(desc, ATA_REG_LBA1, 0);
    io_write8(desc, ATA_REG_LBA2, 0);

    io_write8(desc, ATA_REG_COMMAND, ATA_CMD_IDENTIFY);
    io_wait();

    status = io_read8(desc, ATA_REG_STATUS);
    if (status == 0) {
        state.probed = true;
        return false;
    }

    if (!wait_not_busy(device)) {
        return false;
    }

    uint8_t signature_lba1 = io_read8(desc, ATA_REG_LBA1);
    uint8_t signature_lba2 = io_read8(desc, ATA_REG_LBA2);

    status = io_read8(desc, ATA_REG_STATUS);
    if (status & ATA_SR_ERR) {
        if (signature_lba1 == 0x14 && signature_lba2 == 0xEB) {
        } else if (signature_lba1 == 0x69 && signature_lba2 == 0x96) {
        } else {
        }
        state.probed = true;
        return false;
    }

    if (!wait_drq(device)) {
        return false;
    }

    uint16_t identify_buffer[256];
    read_data(device, identify_buffer, 256);

    swap_bytes(state.identify.model, identify_buffer + 27, 20);
    state.identify.model[40] = '\0';
    trim_string(state.identify.model, 40);

    state.identify.sector_count =
        static_cast<uint32_t>(identify_buffer[60]) |
        (static_cast<uint32_t>(identify_buffer[61]) << 16);

    state.identify.present = true;
    state.probed = true;
    return true;
}

constexpr uint8_t select_lba_prefix(const IdeDeviceDescriptor& desc) {
    return static_cast<uint8_t>(desc.device_select | 0x40);
}

}  // namespace

bool ide_init(IdeDeviceId device) {
    return identify_device(device);
}

bool ide_init() {
    return ide_init(IdeDeviceId::PrimaryMaster);
}

const IdeIdentifyInfo& ide_identify(IdeDeviceId device) {
    identify_device(device);
    return device_state(device).identify;
}

const IdeIdentifyInfo& ide_primary_identify() {
    return ide_identify(IdeDeviceId::PrimaryMaster);
}

const char* ide_device_name(IdeDeviceId device) {
    return device_name(device);
}

IdeStatus ide_read_sectors(IdeDeviceId device, uint32_t lba,
                           uint8_t sector_count, void* buffer) {
    if (!identify_device(device)) {
        return IdeStatus::NoDevice;
    }
    if (!device_state(device).identify.present) {
        return IdeStatus::NoDevice;
    }
    if (sector_count == 0) {
        sector_count = 1;
    }

    uint32_t max_sector = device_state(device).identify.sector_count;
    if (max_sector != 0) {
        uint64_t last_lba =
            static_cast<uint64_t>(lba) + static_cast<uint64_t>(sector_count);
        if (lba >= max_sector || last_lba > max_sector) {
            return IdeStatus::IoError;
        }
    }

    const auto& desc = device_desc(device);
    uint16_t* buffer_words = static_cast<uint16_t*>(buffer);

    if (!wait_not_busy(device)) {
        return IdeStatus::Busy;
    }

    io_write8(desc, ATA_REG_HDDEVSEL,
              select_lba_prefix(desc) | ((lba >> 24) & 0x0F));
    io_wait();
    io_write8(desc, ATA_REG_SECCOUNT0, sector_count);
    io_write8(desc, ATA_REG_LBA0, static_cast<uint8_t>(lba));
    io_write8(desc, ATA_REG_LBA1, static_cast<uint8_t>(lba >> 8));
    io_write8(desc, ATA_REG_LBA2, static_cast<uint8_t>(lba >> 16));

    io_write8(desc, ATA_REG_COMMAND, ATA_CMD_READ_SECTORS);

    for (uint8_t sector = 0; sector < sector_count; ++sector) {
        if (!wait_not_busy(device)) {
            return IdeStatus::Busy;
        }
        if (!wait_drq(device)) {
            return IdeStatus::IoError;
        }

        read_data(device, buffer_words + (sector * 256), 256);
        io_wait();
    }

    return IdeStatus::Ok;
}

IdeStatus ide_read_sectors(uint32_t lba, uint8_t sector_count,
                           void* buffer) {
    return ide_read_sectors(IdeDeviceId::PrimaryMaster, lba, sector_count,
                            buffer);
}

IdeStatus ide_write_sectors(IdeDeviceId device, uint32_t lba,
                            uint8_t sector_count, const void* buffer) {
    if (!identify_device(device)) {
        return IdeStatus::NoDevice;
    }
    if (!device_state(device).identify.present) {
        return IdeStatus::NoDevice;
    }
    if (sector_count == 0) {
        sector_count = 1;
    }

    uint32_t max_sector = device_state(device).identify.sector_count;
    if (max_sector != 0) {
        uint64_t last_lba =
            static_cast<uint64_t>(lba) + static_cast<uint64_t>(sector_count);
        if (lba >= max_sector || last_lba > max_sector) {
            return IdeStatus::IoError;
        }
    }

    const auto& desc = device_desc(device);
    const uint16_t* buffer_words = static_cast<const uint16_t*>(buffer);

    if (!wait_not_busy(device)) {
        return IdeStatus::Busy;
    }

    io_write8(desc, ATA_REG_HDDEVSEL,
              select_lba_prefix(desc) | ((lba >> 24) & 0x0F));
    io_wait();
    io_write8(desc, ATA_REG_SECCOUNT0, sector_count);
    io_write8(desc, ATA_REG_LBA0, static_cast<uint8_t>(lba));
    io_write8(desc, ATA_REG_LBA1, static_cast<uint8_t>(lba >> 8));
    io_write8(desc, ATA_REG_LBA2, static_cast<uint8_t>(lba >> 16));

    io_write8(desc, ATA_REG_COMMAND, ATA_CMD_WRITE_SECTORS);

    for (uint8_t sector = 0; sector < sector_count; ++sector) {
        if (!wait_not_busy(device)) {
            return IdeStatus::Busy;
        }
        if (!wait_drq(device)) {
            return IdeStatus::IoError;
        }

        write_data(device, buffer_words + (sector * 256), 256);
        io_wait();
    }

    if (!wait_not_busy(device)) {
        return IdeStatus::Busy;
    }

    return IdeStatus::Ok;
}

IdeStatus ide_write_sectors(uint32_t lba, uint8_t sector_count,
                            const void* buffer) {
    return ide_write_sectors(IdeDeviceId::PrimaryMaster,
                             lba,
                             sector_count,
                             buffer);
}
