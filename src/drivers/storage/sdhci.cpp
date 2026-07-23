#include "drivers/storage/sdhci.hpp"

#include "arch/x86_64/memory/paging.hpp"
#include "drivers/log/logging.hpp"
#include "drivers/pci/pci.hpp"
#include "kernel/memory/physical_allocator.hpp"
#include "lib/mem.hpp"

namespace sdhci {
namespace {

constexpr size_t kMaxControllers = 4;
constexpr size_t kMaxDevices = 4;
constexpr size_t kPageSize = 4096;
constexpr uint64_t kMmioVirtBase = 0xFFFFE34000000000ull;
constexpr uint64_t kMmioWindowSize = 1024ull * 1024;
constexpr uint64_t kRegisterMapLength = 0x1000;

constexpr uint8_t kPciCommandMemorySpace = 1u << 1;
constexpr uint8_t kPciCommandBusMaster = 1u << 2;

constexpr uint16_t RegBlockSize = 0x04;
constexpr uint16_t RegBlockCount = 0x06;
constexpr uint16_t RegArgument2 = 0x00;
constexpr uint16_t RegArgument = 0x08;
constexpr uint16_t RegTransferMode = 0x0C;
constexpr uint16_t RegCommand = 0x0E;
constexpr uint16_t RegResponse = 0x10;
constexpr uint16_t RegBufferData = 0x20;
constexpr uint16_t RegPresentState = 0x24;
constexpr uint16_t RegHostControl1 = 0x28;
constexpr uint16_t RegPowerControl = 0x29;
constexpr uint16_t RegClockControl = 0x2C;
constexpr uint16_t RegTimeoutControl = 0x2E;
constexpr uint16_t RegSoftwareReset = 0x2F;
constexpr uint16_t RegNormalIntStatus = 0x30;
constexpr uint16_t RegErrorIntStatus = 0x32;
constexpr uint16_t RegNormalIntEnable = 0x34;
constexpr uint16_t RegErrorIntEnable = 0x36;
constexpr uint16_t RegNormalSignalEnable = 0x38;
constexpr uint16_t RegErrorSignalEnable = 0x3A;
constexpr uint16_t RegCapabilities = 0x40;
constexpr uint16_t RegAdmaSystemAddress = 0x58;
constexpr uint16_t RegHostVersion = 0xFE;

constexpr uint32_t PresentCommandInhibit = 1u << 0;
constexpr uint32_t PresentDataInhibit = 1u << 1;

constexpr uint16_t NormalCommandComplete = 1u << 0;
constexpr uint16_t NormalTransferComplete = 1u << 1;
constexpr uint16_t NormalBufferWriteReady = 1u << 4;
constexpr uint16_t NormalBufferReadReady = 1u << 5;
constexpr uint16_t NormalError = 1u << 15;

constexpr uint16_t TransferBlockCountEnable = 1u << 1;
constexpr uint16_t TransferAutoCmd12 = 1u << 2;
constexpr uint16_t TransferAutoCmd23 = 2u << 2;
constexpr uint16_t TransferDmaEnable = 1u << 0;
constexpr uint16_t TransferRead = 1u << 4;
constexpr uint16_t TransferMultiBlock = 1u << 5;

constexpr uint16_t CommandRespNone = 0;
constexpr uint16_t CommandResp136 = 1u;
constexpr uint16_t CommandResp48 = 2u;
constexpr uint16_t CommandResp48Busy = 3u;
constexpr uint16_t CommandCrcCheck = 1u << 3;
constexpr uint16_t CommandIndexCheck = 1u << 4;
constexpr uint16_t CommandDataPresent = 1u << 5;

constexpr uint8_t HostControl1FourBit = 1u << 1;
constexpr uint8_t HostControl1HighSpeed = 1u << 2;
constexpr uint8_t HostControl1EightBit = 1u << 5;
constexpr uint8_t HostControl1DmaSelectMask = 0x18;
constexpr uint8_t HostControl1DmaSelectAdma2 = 0x10;
constexpr uint8_t HostControl1DmaSelectAdma2_64 = 0x18;

constexpr uint8_t ResetAll = 1u << 0;
constexpr uint8_t ResetCommand = 1u << 1;
constexpr uint8_t ResetData = 1u << 2;

constexpr uint64_t CapabilityVoltage33 = 1ull << 24;
constexpr uint64_t CapabilityVoltage30 = 1ull << 25;
constexpr uint64_t CapabilityVoltage18 = 1ull << 26;
constexpr uint64_t CapabilityAdma2 = 1ull << 19;
constexpr uint64_t CapabilityAdma64 = 1ull << 28;

constexpr uint8_t PowerOn = 1u << 0;
constexpr uint8_t PowerVoltage18 = 0x0Au;
constexpr uint8_t PowerVoltage30 = 0x0Cu;
constexpr uint8_t PowerVoltage33 = 0x0Eu;

constexpr uint32_t OcrBusy = 1u << 31;
constexpr uint32_t OcrSectorMode = 1u << 30;
constexpr uint32_t OcrVoltageMask = 0x00FF8000u;

constexpr uint8_t CmdGoIdle = 0;
constexpr uint8_t CmdSendOpCond = 1;
constexpr uint8_t CmdAllSendCid = 2;
constexpr uint8_t CmdSetRelativeAddr = 3;
constexpr uint8_t CmdSwitch = 6;
constexpr uint8_t CmdSelectCard = 7;
constexpr uint8_t CmdSendExtCsd = 8;
constexpr uint8_t CmdSendCsd = 9;
constexpr uint8_t CmdSetBlockLength = 16;
constexpr uint8_t CmdReadSingleBlock = 17;
constexpr uint8_t CmdReadMultipleBlock = 18;
constexpr uint8_t CmdSetBlockCount = 23;
constexpr uint8_t CmdWriteSingleBlock = 24;
constexpr uint8_t CmdWriteMultipleBlock = 25;
constexpr uint8_t CmdAppCommand = 55;
constexpr uint8_t AcmdSetBusWidth = 6;
constexpr uint8_t AcmdSdSendOpCond = 41;

constexpr uint32_t kWaitSpins = 1000000;
constexpr uint32_t kCommandRetries = 3;
constexpr uint32_t kInitRetries = 1000;
constexpr uint16_t kDefaultRca = 1;

constexpr uint8_t ExtCsdBusWidth = 183;
constexpr uint8_t ExtCsdHsTiming = 185;
constexpr uint8_t ExtCsdBusWidth4Bit = 1;
constexpr uint8_t ExtCsdBusWidth8Bit = 2;
constexpr uint8_t ExtCsdHsTimingHighSpeed = 1;
constexpr uint32_t MmcSwitchAccessWriteByte = 3u << 24;
constexpr uint32_t SdOcrHighCapacity = 1u << 30;
constexpr uint32_t SdCheckPattern = 0xAAu;
constexpr uint32_t SdVoltageAccepted = 1u << 8;

enum class CardKind {
    Unknown,
    Mmc,
    Sd,
};

struct [[gnu::packed]] Adma2Descriptor32 {
    uint16_t attr;
    uint16_t length;
    uint32_t address;
};

struct [[gnu::packed]] Adma2Descriptor64 {
    uint16_t attr;
    uint16_t length;
    uint32_t address_low;
    uint32_t address_high;
};

constexpr uint16_t AdmaAttrValid = 1u << 0;
constexpr uint16_t AdmaAttrEnd = 1u << 1;
constexpr uint16_t AdmaAttrTransfer = 2u << 4;

struct ControllerState {
    bool used;
    pci::PciDevice pci_device;
    volatile uint8_t* regs;
    uint64_t regs_phys;
    uint32_t base_clock_hz;
    uint64_t capabilities;
    uint8_t selected_power;
};

struct DeviceState {
    bool used;
    bool present;
    ControllerState* controller;
    IdentifyInfo identify;
    char name[16];
    uint16_t rca;
    CardKind kind;
    bool sector_addressed;
    bool adma2_supported;
    bool adma2_64bit;
    bool adma2_failed;
    bool auto_cmd23_failed;
    uint64_t adma_desc_phys;
    void* adma_desc;
    volatile int lock;
};

ControllerState g_controllers[kMaxControllers]{};
DeviceState g_devices[kMaxDevices]{};
size_t g_device_count = 0;
bool g_initialized = false;
uint64_t g_mmio_next_virt = kMmioVirtBase;
alignas(4) uint8_t g_ext_csd[512];

constexpr IdentifyInfo kEmptyIdentifyInfo = {false, "", 0, false};

void cpu_relax() {
    asm volatile("pause");
}

void lock_device(DeviceState& device) {
    while (__atomic_test_and_set(&device.lock, __ATOMIC_ACQUIRE)) {
        cpu_relax();
    }
}

void unlock_device(DeviceState& device) {
    __atomic_clear(&device.lock, __ATOMIC_RELEASE);
}

uint64_t align_down_u64(uint64_t value, uint64_t alignment) {
    return value & ~(alignment - 1);
}

uint64_t align_up_u64(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

uint8_t read8(const ControllerState& controller, uint16_t offset) {
    return *(controller.regs + offset);
}

uint16_t read16(const ControllerState& controller, uint16_t offset) {
    return *reinterpret_cast<volatile uint16_t*>(controller.regs + offset);
}

uint32_t read32(const ControllerState& controller, uint16_t offset) {
    return *reinterpret_cast<volatile uint32_t*>(controller.regs + offset);
}

void write8(const ControllerState& controller, uint16_t offset, uint8_t value) {
    *(controller.regs + offset) = value;
}

void write16(const ControllerState& controller, uint16_t offset, uint16_t value) {
    *reinterpret_cast<volatile uint16_t*>(controller.regs + offset) = value;
}

void write32(const ControllerState& controller, uint16_t offset, uint32_t value) {
    *reinterpret_cast<volatile uint32_t*>(controller.regs + offset) = value;
}

uint64_t alloc_zeroed_pages(size_t page_count) {
    if (page_count == 0) {
        return 0;
    }
    uint64_t phys = memory::alloc_kernel_block_pages(page_count);
    if (phys == 0) {
        return 0;
    }
    void* virt = paging_phys_to_virt(phys);
    if (virt == nullptr) {
        memory::free_kernel_block(phys);
        return 0;
    }
    memset(virt, 0, page_count * kPageSize);
    return phys;
}

bool set_clock(ControllerState& controller, uint32_t target_hz);
Status command_with_retries(ControllerState& controller,
                            uint8_t command,
                            uint32_t argument,
                            bool data_present,
                            bool read_transfer,
                            uint32_t* out_response = nullptr,
                            bool multi_block = false,
                            bool dma_transfer = false,
                            bool auto_cmd23 = false,
                            uint8_t sector_count = 0,
                            bool auto_cmd12 = false);

uint64_t pci_bar_base(const pci::PciDevice& device, uint8_t bar_index) {
    if (bar_index >= 6) {
        return 0;
    }

    uint8_t reg = static_cast<uint8_t>(0x10 + (bar_index * 4));
    uint32_t low = pci::read_config32(device, reg);
    if ((low & 0x1u) != 0) {
        return 0;
    }

    uint64_t base = static_cast<uint64_t>(low & ~0xFu);
    uint32_t bar_type = (low >> 1) & 0x3u;
    if (bar_type == 0x2u) {
        if (bar_index + 1 >= 6) {
            return 0;
        }
        uint32_t high = pci::read_config32(device, static_cast<uint8_t>(reg + 4));
        base |= static_cast<uint64_t>(high) << 32;
    }
    return base;
}

uint64_t find_mmio_bar(const pci::PciDevice& device, uint8_t& out_bar_index) {
    for (uint8_t bar = 0; bar < 6; ++bar) {
        uint64_t base = pci_bar_base(device, bar);
        if (base != 0) {
            out_bar_index = bar;
            return base;
        }
    }
    out_bar_index = 0xFF;
    return 0;
}

volatile uint8_t* map_mmio_range(uint64_t phys_base, uint64_t length) {
    if (phys_base == 0 || length == 0) {
        return nullptr;
    }

    uint64_t page_phys = align_down_u64(phys_base, kPageSize);
    uint64_t page_end = align_up_u64(phys_base + length, kPageSize);
    size_t page_count =
        static_cast<size_t>((page_end - page_phys) / kPageSize);

    uint64_t virt_base = g_mmio_next_virt;
    uint64_t virt_end =
        virt_base + static_cast<uint64_t>(page_count) * kPageSize;
    if (virt_end - kMmioVirtBase > kMmioWindowSize) {
        log_message(LogLevel::Warn,
                    "sdhci: MMIO window exhausted while mapping %016llx",
                    static_cast<unsigned long long>(phys_base));
        return nullptr;
    }

    const uint64_t flags = PAGE_FLAG_WRITE | PAGE_FLAG_WRITE_THROUGH |
                           PAGE_FLAG_CACHE_DISABLE | PAGE_FLAG_NO_EXECUTE;
    for (size_t i = 0; i < page_count; ++i) {
        uint64_t phys = page_phys + static_cast<uint64_t>(i) * kPageSize;
        uint64_t virt = virt_base + static_cast<uint64_t>(i) * kPageSize;
        if (!paging_map_page(virt, phys, flags)) {
            log_message(LogLevel::Warn,
                        "sdhci: failed to map MMIO page phys=%016llx",
                        static_cast<unsigned long long>(phys));
            return nullptr;
        }
    }

    g_mmio_next_virt = virt_end;
    return reinterpret_cast<volatile uint8_t*>(virt_base + (phys_base - page_phys));
}

bool wait_reset_clear(const ControllerState& controller, uint8_t mask) {
    for (uint32_t spin = 0; spin < kWaitSpins; ++spin) {
        if ((read8(controller, RegSoftwareReset) & mask) == 0) {
            return true;
        }
        cpu_relax();
    }
    return false;
}

bool reset_lines(const ControllerState& controller, uint8_t mask) {
    write8(controller, RegSoftwareReset, mask);
    return wait_reset_clear(controller, mask);
}

bool supports_auto_cmd23(const ControllerState& controller) {
    return (read16(controller, RegHostVersion) & 0xFFu) >= 3u;
}

bool wait_present_clear(const ControllerState& controller, uint32_t mask) {
    for (uint32_t spin = 0; spin < kWaitSpins; ++spin) {
        if ((read32(controller, RegPresentState) & mask) == 0) {
            return true;
        }
        cpu_relax();
    }
    return false;
}

uint32_t response32(const ControllerState& controller) {
    return read32(controller, RegResponse);
}

bool wait_normal_status(const ControllerState& controller,
                        uint16_t wanted,
                        uint16_t& normal,
                        uint16_t& error) {
    for (uint32_t spin = 0; spin < kWaitSpins; ++spin) {
        normal = read16(controller, RegNormalIntStatus);
        error = read16(controller, RegErrorIntStatus);
        if ((normal & NormalError) != 0 || error != 0) {
            return false;
        }
        if ((normal & wanted) == wanted) {
            return true;
        }
        cpu_relax();
    }
    return false;
}

Status issue_command(ControllerState& controller,
                     uint8_t command,
                     uint32_t argument,
                     uint16_t response_flags,
                     bool data_present,
                     bool read_transfer,
                     bool multi_block,
                     bool dma_transfer,
                     bool auto_cmd23,
                     uint8_t sector_count,
                     bool auto_cmd12,
                     uint32_t* out_response = nullptr) {
    uint32_t inhibit = PresentCommandInhibit;
    if (data_present) {
        inhibit |= PresentDataInhibit;
    }
    if (!wait_present_clear(controller, inhibit)) {
        reset_lines(controller, ResetCommand | ResetData);
        return Status::Busy;
    }

    write16(controller, RegNormalIntStatus, 0xFFFFu);
    write16(controller, RegErrorIntStatus, 0xFFFFu);

    uint16_t transfer = 0;
    if (data_present) {
        transfer |= TransferBlockCountEnable;
        if (dma_transfer) {
            transfer |= TransferDmaEnable;
        }
        if (auto_cmd23 && multi_block && sector_count != 0) {
            write32(controller, RegArgument2, sector_count);
            transfer |= TransferAutoCmd23;
        } else if (auto_cmd12 && multi_block) {
            transfer |= TransferAutoCmd12;
        }
        if (read_transfer) {
            transfer |= TransferRead;
        }
        if (multi_block) {
            transfer |= TransferMultiBlock;
        }
    }
    write16(controller, RegTransferMode, transfer);
    write32(controller, RegArgument, argument);

    uint16_t command_reg =
        static_cast<uint16_t>((static_cast<uint16_t>(command) << 8) |
                             response_flags |
                              (data_present ? CommandDataPresent : 0));
    write16(controller, RegCommand, command_reg);

    uint16_t normal = 0;
    uint16_t error = 0;
    if (!wait_normal_status(controller, NormalCommandComplete, normal, error)) {
        log_message(LogLevel::Warn,
                    "sdhci: CMD%u failed normal=%04x error=%04x present=%08x",
                    static_cast<unsigned int>(command),
                    static_cast<unsigned int>(normal),
                    static_cast<unsigned int>(error),
                    static_cast<unsigned int>(read32(controller, RegPresentState)));
        reset_lines(controller, ResetCommand | (data_present ? ResetData : 0));
        return error != 0 ? Status::IoError : Status::Timeout;
    }

    write16(controller, RegNormalIntStatus, NormalCommandComplete);
    if (out_response != nullptr) {
        *out_response = response32(controller);
    }
    return Status::Ok;
}

Status read_data_payload(ControllerState& controller, uint8_t* buffer) {
    uint16_t normal = 0;
    uint16_t error = 0;
    if (!wait_normal_status(controller, NormalBufferReadReady, normal, error)) {
        reset_lines(controller, ResetData);
        return error != 0 ? Status::IoError : Status::Timeout;
    }
    write16(controller, RegNormalIntStatus, NormalBufferReadReady);

    if ((reinterpret_cast<uintptr_t>(buffer) & (sizeof(uint32_t) - 1)) == 0) {
        auto* words = reinterpret_cast<uint32_t*>(buffer);
        for (size_t i = 0; i < 512 / sizeof(uint32_t); ++i) {
            words[i] = read32(controller, RegBufferData);
        }
    } else {
        for (size_t i = 0; i < 512; i += sizeof(uint32_t)) {
            uint32_t word = read32(controller, RegBufferData);
            buffer[i] = static_cast<uint8_t>(word);
            buffer[i + 1] = static_cast<uint8_t>(word >> 8);
            buffer[i + 2] = static_cast<uint8_t>(word >> 16);
            buffer[i + 3] = static_cast<uint8_t>(word >> 24);
        }
    }

    return Status::Ok;
}

Status write_data_payload(ControllerState& controller, const uint8_t* buffer) {
    uint16_t normal = 0;
    uint16_t error = 0;
    if (!wait_normal_status(controller, NormalBufferWriteReady, normal, error)) {
        reset_lines(controller, ResetData);
        return error != 0 ? Status::IoError : Status::Timeout;
    }
    write16(controller, RegNormalIntStatus, NormalBufferWriteReady);

    if ((reinterpret_cast<uintptr_t>(buffer) & (sizeof(uint32_t) - 1)) == 0) {
        const auto* words = reinterpret_cast<const uint32_t*>(buffer);
        for (size_t i = 0; i < 512 / sizeof(uint32_t); ++i) {
            write32(controller, RegBufferData, words[i]);
        }
    } else {
        for (size_t i = 0; i < 512; i += sizeof(uint32_t)) {
            uint32_t word = static_cast<uint32_t>(buffer[i]) |
                            (static_cast<uint32_t>(buffer[i + 1]) << 8) |
                            (static_cast<uint32_t>(buffer[i + 2]) << 16) |
                            (static_cast<uint32_t>(buffer[i + 3]) << 24);
            write32(controller, RegBufferData, word);
        }
    }

    return Status::Ok;
}

Status wait_transfer_complete(ControllerState& controller) {
    uint16_t normal = 0;
    uint16_t error = 0;
    if (!wait_normal_status(controller, NormalTransferComplete, normal, error)) {
        reset_lines(controller, ResetData);
        return error != 0 ? Status::IoError : Status::Timeout;
    }
    write16(controller, RegNormalIntStatus, NormalTransferComplete);
    return Status::Ok;
}

bool build_adma2_table(DeviceState& device, void* buffer, size_t byte_count) {
    if (device.adma_desc == nullptr || buffer == nullptr || byte_count == 0) {
        return false;
    }

    memset(device.adma_desc, 0, kPageSize);
    uintptr_t virt = reinterpret_cast<uintptr_t>(buffer);
    size_t remaining = byte_count;
    size_t desc_index = 0;
    size_t max_descriptors = device.adma2_64bit
                                 ? kPageSize / sizeof(Adma2Descriptor64)
                                 : kPageSize / sizeof(Adma2Descriptor32);

    while (remaining > 0) {
        if (desc_index >= max_descriptors) {
            return false;
        }

        uint64_t phys = 0;
        if (!paging_resolve_cr3(paging_kernel_cr3(), virt, phys)) {
            return false;
        }

        size_t page_offset = static_cast<size_t>(virt & (kPageSize - 1));
        size_t chunk = kPageSize - page_offset;
        if (chunk > remaining) {
            chunk = remaining;
        }
        if (chunk > 0xFFFFu) {
            chunk = 0xFFFFu;
        }
        if (!device.adma2_64bit && (phys >> 32) != 0) {
            return false;
        }

        uint16_t attr = static_cast<uint16_t>(AdmaAttrValid | AdmaAttrTransfer);
        if (device.adma2_64bit) {
            auto* desc = static_cast<Adma2Descriptor64*>(device.adma_desc);
            desc[desc_index].attr = attr;
            desc[desc_index].length = static_cast<uint16_t>(chunk);
            desc[desc_index].address_low = static_cast<uint32_t>(phys);
            desc[desc_index].address_high = static_cast<uint32_t>(phys >> 32);
        } else {
            auto* desc = static_cast<Adma2Descriptor32*>(device.adma_desc);
            desc[desc_index].attr = attr;
            desc[desc_index].length = static_cast<uint16_t>(chunk);
            desc[desc_index].address = static_cast<uint32_t>(phys);
        }
        ++desc_index;

        virt += chunk;
        remaining -= chunk;
    }

    if (desc_index == 0) {
        return false;
    }
    if (device.adma2_64bit) {
        auto* desc = static_cast<Adma2Descriptor64*>(device.adma_desc);
        desc[desc_index - 1].attr |= AdmaAttrEnd;
    } else {
        auto* desc = static_cast<Adma2Descriptor32*>(device.adma_desc);
        desc[desc_index - 1].attr |= AdmaAttrEnd;
    }
    return true;
}

Status transfer_data_dma(DeviceState& device,
                         uint8_t command,
                         uint32_t argument,
                         bool read_transfer,
                         bool multi_block,
                         void* buffer,
                         uint8_t sector_count,
                         bool auto_cmd23,
                         bool auto_cmd12) {
    if (!device.adma2_supported || device.adma2_failed || sector_count == 0) {
        return Status::IoError;
    }

    size_t byte_count = static_cast<size_t>(sector_count) * 512u;
    if (!build_adma2_table(device, buffer, byte_count)) {
        device.adma2_failed = true;
        log_message(LogLevel::Warn,
                    "sdhci: ADMA2 table build failed, falling back to PIO");
        return Status::IoError;
    }

    ControllerState& controller = *device.controller;
    uint8_t control = read8(controller, RegHostControl1);
    write8(controller,
           RegHostControl1,
           static_cast<uint8_t>((control & ~HostControl1DmaSelectMask) |
                                (device.adma2_64bit
                                     ? HostControl1DmaSelectAdma2_64
                                     : HostControl1DmaSelectAdma2)));
    write32(controller,
            RegAdmaSystemAddress,
            static_cast<uint32_t>(device.adma_desc_phys));
    write32(controller,
            RegAdmaSystemAddress + 4,
            static_cast<uint32_t>(device.adma_desc_phys >> 32));

    Status result = command_with_retries(controller,
                                         command,
                                         argument,
                                         true,
                                         read_transfer,
                                         nullptr,
                                         multi_block,
                                         true,
                                         auto_cmd23,
                                         sector_count,
                                         auto_cmd12);
    if (result == Status::Ok) {
        result = wait_transfer_complete(controller);
    }
    if (result != Status::Ok) {
        reset_lines(controller, ResetData);
        if (auto_cmd23) {
            device.auto_cmd23_failed = true;
        } else {
            device.adma2_failed = true;
        }
        log_message(LogLevel::Warn,
                    "sdhci: ADMA2 transfer failed status=%d, falling back",
                    static_cast<int>(result));
    }
    write8(controller, RegHostControl1, control);
    return result;
}

Status read_data_block(ControllerState& controller, uint8_t* buffer) {
    Status status = read_data_payload(controller, buffer);
    if (status != Status::Ok) {
        return status;
    }
    return wait_transfer_complete(controller);
}

Status write_data_block(ControllerState& controller, const uint8_t* buffer) {
    Status status = write_data_payload(controller, buffer);
    if (status != Status::Ok) {
        return status;
    }
    return wait_transfer_complete(controller);
}

uint16_t response_for(uint8_t command) {
    switch (command) {
        case CmdGoIdle:
            return CommandRespNone;
        case CmdAllSendCid:
        case CmdSendCsd:
            return static_cast<uint16_t>(CommandResp136 | CommandCrcCheck);
        case CmdSendOpCond:
            return CommandResp48;
        case CmdSwitch:
            return static_cast<uint16_t>(CommandResp48Busy | CommandCrcCheck |
                                         CommandIndexCheck);
        default:
            return static_cast<uint16_t>(CommandResp48 | CommandCrcCheck |
                                         CommandIndexCheck);
    }
}

uint16_t app_response_for(uint8_t command) {
    if (command == AcmdSdSendOpCond) {
        return CommandResp48;
    }
    return static_cast<uint16_t>(CommandResp48 | CommandCrcCheck |
                                 CommandIndexCheck);
}

Status command_with_retries(ControllerState& controller,
                            uint8_t command,
                            uint32_t argument,
                            bool data_present,
                            bool read_transfer,
                            uint32_t* out_response,
                            bool multi_block,
                            bool dma_transfer,
                            bool auto_cmd23,
                            uint8_t sector_count,
                            bool auto_cmd12) {
    Status last = Status::IoError;
    for (uint32_t attempt = 0; attempt < kCommandRetries; ++attempt) {
        last = issue_command(controller,
                             command,
                             argument,
                             response_for(command),
                             data_present,
                             read_transfer,
                             multi_block,
                             dma_transfer,
                             auto_cmd23,
                             sector_count,
                             auto_cmd12,
                             out_response);
        if (last == Status::Ok) {
            return Status::Ok;
        }
    }
    return last;
}

Status set_block_count(ControllerState& controller, uint8_t sector_count) {
    return command_with_retries(controller,
                                CmdSetBlockCount,
                                sector_count,
                                false,
                                false);
}

Status transfer_data_pio(ControllerState& controller,
                         uint8_t command,
                         uint32_t argument,
                         bool read_transfer,
                         bool multi_block,
                         void* buffer,
                         uint8_t sector_count,
                         bool auto_cmd23,
                         bool auto_cmd12) {
    Status result = command_with_retries(controller,
                                         command,
                                         argument,
                                         true,
                                         read_transfer,
                                         nullptr,
                                         multi_block,
                                         false,
                                         auto_cmd23,
                                         sector_count,
                                         auto_cmd12);
    if (result != Status::Ok) {
        return result;
    }

    auto* bytes = static_cast<uint8_t*>(buffer);
    for (uint8_t i = 0; result == Status::Ok && i < sector_count; ++i) {
        uint8_t* sector = bytes + (static_cast<size_t>(i) * 512u);
        result = read_transfer ? read_data_payload(controller, sector)
                               : write_data_payload(controller, sector);
    }
    if (result == Status::Ok) {
        result = wait_transfer_complete(controller);
    }
    return result;
}

Status switch_ext_csd(ControllerState& controller, uint8_t index, uint8_t value) {
    uint32_t argument = MmcSwitchAccessWriteByte |
                        (static_cast<uint32_t>(index) << 16) |
                        (static_cast<uint32_t>(value) << 8);
    Status status = command_with_retries(controller,
                                        CmdSwitch,
                                        argument,
                                        false,
                                        false);
    if (status != Status::Ok) {
        return status;
    }
    if (!wait_present_clear(controller, PresentDataInhibit)) {
        reset_lines(controller, ResetData);
        return Status::Timeout;
    }
    return Status::Ok;
}

bool enable_4bit_bus(ControllerState& controller) {
    Status status = switch_ext_csd(controller, ExtCsdBusWidth, ExtCsdBusWidth4Bit);
    if (status != Status::Ok) {
        log_message(LogLevel::Warn,
                    "sdhci: eMMC 4-bit bus switch failed status=%d",
                    static_cast<int>(status));
        return false;
    }

    write8(controller,
           RegHostControl1,
           static_cast<uint8_t>((read8(controller, RegHostControl1) &
                                 ~HostControl1EightBit) |
                                HostControl1FourBit));
    log_message(LogLevel::Info, "sdhci: enabled 4-bit bus");
    return true;
}

bool enable_8bit_bus(ControllerState& controller) {
    Status status = switch_ext_csd(controller, ExtCsdBusWidth, ExtCsdBusWidth8Bit);
    if (status != Status::Ok) {
        log_message(LogLevel::Warn,
                    "sdhci: eMMC 8-bit bus switch failed status=%d",
                    static_cast<int>(status));
        return false;
    }

    write8(controller,
           RegHostControl1,
           static_cast<uint8_t>((read8(controller, RegHostControl1) &
                                 ~HostControl1FourBit) |
                                HostControl1EightBit));
    log_message(LogLevel::Info, "sdhci: enabled 8-bit bus");
    return true;
}

bool enable_high_speed(ControllerState& controller) {
    Status status =
        switch_ext_csd(controller, ExtCsdHsTiming, ExtCsdHsTimingHighSpeed);
    if (status != Status::Ok) {
        log_message(LogLevel::Warn,
                    "sdhci: eMMC high-speed switch failed status=%d",
                    static_cast<int>(status));
        return false;
    }

    write8(controller,
           RegHostControl1,
           static_cast<uint8_t>(read8(controller, RegHostControl1) |
                                HostControl1HighSpeed));
    if (!set_clock(controller, 50000000)) {
        log_message(LogLevel::Warn,
                    "sdhci: 50 MHz clock setup failed after high-speed switch");
        return false;
    }
    log_message(LogLevel::Info, "sdhci: enabled high-speed timing");
    return true;
}

void init_adma2(DeviceState& device) {
    ControllerState& controller = *device.controller;
    if ((controller.capabilities & CapabilityAdma2) == 0) {
        log_message(LogLevel::Info, "sdhci: ADMA2 not advertised");
        return;
    }
    bool use_64bit = (controller.capabilities & CapabilityAdma64) != 0;

    uint64_t phys = alloc_zeroed_pages(1);
    if (phys == 0) {
        log_message(LogLevel::Warn, "sdhci: ADMA2 descriptor allocation failed");
        return;
    }
    if (!use_64bit && (phys >> 32) != 0) {
        memory::free_kernel_block(phys);
        log_message(LogLevel::Warn,
                    "sdhci: ADMA2 descriptor page above 4GB, using PIO");
        return;
    }
    void* desc = paging_phys_to_virt(phys);
    if (desc == nullptr) {
        memory::free_kernel_block(phys);
        return;
    }

    device.adma_desc_phys = phys;
    device.adma_desc = desc;
    device.adma2_supported = true;
    device.adma2_64bit = use_64bit;
    device.adma2_failed = false;
    log_message(LogLevel::Info,
                "sdhci: ADMA2 enabled (%s-bit)",
                use_64bit ? "64" : "32");
}

bool set_clock(ControllerState& controller, uint32_t target_hz) {
    write16(controller, RegClockControl, 0);

    uint32_t base_hz = controller.base_clock_hz;
    if (base_hz == 0) {
        base_hz = 50000000;
    }

    uint16_t divisor = 0;
    if (target_hz < base_hz) {
        for (uint32_t div = 2; div <= 2046; div += 2) {
            if ((base_hz / div) <= target_hz) {
                divisor = static_cast<uint16_t>(div / 2);
                break;
            }
        }
        if (divisor == 0) {
            divisor = 0x3FF;
        }
    }

    uint16_t clock = static_cast<uint16_t>(((divisor & 0xFFu) << 8) |
                                           ((divisor & 0x300u) >> 2) |
                                           1u);
    write16(controller, RegClockControl, clock);
    for (uint32_t spin = 0; spin < kWaitSpins; ++spin) {
        if ((read16(controller, RegClockControl) & (1u << 1)) != 0) {
            write16(controller, RegClockControl,
                    static_cast<uint16_t>(read16(controller, RegClockControl) |
                                          (1u << 2)));
            return true;
        }
        cpu_relax();
    }
    return false;
}

bool power_on(ControllerState& controller) {
    uint8_t candidates[3]{};
    size_t candidate_count = 0;

    if ((controller.capabilities & CapabilityVoltage33) != 0) {
        candidates[candidate_count++] = PowerVoltage33;
    }
    if ((controller.capabilities & CapabilityVoltage30) != 0) {
        candidates[candidate_count++] = PowerVoltage30;
    }
    if ((controller.capabilities & CapabilityVoltage18) != 0) {
        candidates[candidate_count++] = PowerVoltage18;
    }

    if (candidate_count == 0) {
        log_message(LogLevel::Warn,
                    "sdhci: controller reports no voltage caps, trying 1.8V/3.3V fallback caps=%08x:%08x",
                    static_cast<unsigned int>(controller.capabilities >> 32),
                    static_cast<unsigned int>(controller.capabilities));
        candidates[candidate_count++] = PowerVoltage18;
        candidates[candidate_count++] = PowerVoltage33;
    }

    for (size_t idx = 0; idx < candidate_count; ++idx) {
        write8(controller, RegPowerControl, 0);
        controller.selected_power = static_cast<uint8_t>(candidates[idx] | PowerOn);
        write8(controller, RegPowerControl, controller.selected_power);
        for (uint32_t spin = 0; spin < 10000; ++spin) {
            if ((read8(controller, RegPowerControl) & PowerOn) != 0) {
                log_message(LogLevel::Info,
                            "sdhci: powered controller power=%02x",
                            static_cast<unsigned int>(read8(controller, RegPowerControl)));
                return true;
            }
            cpu_relax();
        }
    }

    log_message(LogLevel::Warn,
                "sdhci: power-on timeout power=%02x",
                static_cast<unsigned int>(read8(controller, RegPowerControl)));
    return false;
}

uint32_t read_u32_le(const uint8_t* data) {
    return static_cast<uint32_t>(data[0]) |
           (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) |
           (static_cast<uint32_t>(data[3]) << 24);
}

uint32_t read_bits_be(const uint8_t* data, size_t msb, size_t width) {
    uint32_t value = 0;
    for (size_t i = 0; i < width; ++i) {
        size_t bit = msb - i;
        size_t byte_index = (127u - bit) / 8u;
        size_t bit_index = bit % 8u;
        value <<= 1;
        value |= (data[byte_index] >> bit_index) & 1u;
    }
    return value;
}

void read_response_136(const ControllerState& controller, uint8_t* out) {
    uint32_t words[4] = {
        read32(controller, RegResponse),
        read32(controller, RegResponse + 4),
        read32(controller, RegResponse + 8),
        read32(controller, RegResponse + 12),
    };

    for (size_t word = 0; word < 4; ++word) {
        uint32_t value = words[3 - word];
        out[word * 4] = static_cast<uint8_t>(value >> 24);
        out[word * 4 + 1] = static_cast<uint8_t>(value >> 16);
        out[word * 4 + 2] = static_cast<uint8_t>(value >> 8);
        out[word * 4 + 3] = static_cast<uint8_t>(value);
    }
}

uint64_t sectors_from_sd_csd(const uint8_t* csd) {
    uint32_t structure = read_bits_be(csd, 127, 2);
    if (structure == 1) {
        uint32_t c_size = read_bits_be(csd, 69, 22);
        return static_cast<uint64_t>(c_size + 1u) * 1024ull;
    }
    if (structure == 0) {
        uint32_t read_bl_len = read_bits_be(csd, 83, 4);
        uint32_t c_size = read_bits_be(csd, 73, 12);
        uint32_t c_size_mult = read_bits_be(csd, 49, 3);
        uint64_t block_len = 1ull << read_bl_len;
        uint64_t block_count =
            static_cast<uint64_t>(c_size + 1u) << (c_size_mult + 2u);
        return (block_count * block_len) / 512ull;
    }
    return 0;
}

void copy_printable(char* out, size_t out_size, const uint8_t* data, size_t len) {
    if (out == nullptr || out_size == 0) {
        return;
    }
    size_t idx = 0;
    for (size_t i = 0; i < len && idx + 1 < out_size; ++i) {
        char ch = static_cast<char>(data[i]);
        out[idx++] = (ch >= 32 && ch <= 126) ? ch : '?';
    }
    while (idx > 0 && out[idx - 1] == ' ') {
        --idx;
    }
    out[idx] = '\0';
}

bool prepare_card(ControllerState& controller) {
    if (!reset_lines(controller, ResetAll)) {
        return false;
    }

    write8(controller, RegHostControl1, 0);
    write16(controller, RegNormalIntEnable, 0xFFFFu);
    write16(controller, RegErrorIntEnable, 0xFFFFu);
    write16(controller, RegNormalSignalEnable, 0);
    write16(controller, RegErrorSignalEnable, 0);
    write8(controller, RegTimeoutControl, 0x0E);

    uint64_t caps = controller.capabilities;
    uint32_t base_mhz = static_cast<uint32_t>((caps >> 8) & 0xFFu);
    if (base_mhz == 0) {
        base_mhz = static_cast<uint32_t>((caps >> 8) & 0x3FFu);
    }
    controller.base_clock_hz = base_mhz * 1000000u;

    log_message(LogLevel::Info,
                "sdhci: caps=%08x:%08x hostver=%04x base_clock=%u MHz",
                static_cast<unsigned int>(caps >> 32),
                static_cast<unsigned int>(caps),
                static_cast<unsigned int>(read16(controller, RegHostVersion)),
                static_cast<unsigned int>(base_mhz));

    if (!power_on(controller) || !set_clock(controller, 400000)) {
        return false;
    }

    return command_with_retries(controller, CmdGoIdle, 0, false, false) ==
           Status::Ok;
}

Status app_command(ControllerState& controller,
                   uint16_t rca,
                   uint8_t command,
                   uint32_t argument,
                   uint32_t* out_response = nullptr) {
    Status status = command_with_retries(controller,
                                         CmdAppCommand,
                                         static_cast<uint32_t>(rca) << 16,
                                         false,
                                         false);
    if (status != Status::Ok) {
        return status;
    }
    Status last = Status::IoError;
    for (uint32_t attempt = 0; attempt < kCommandRetries; ++attempt) {
        last = issue_command(controller,
                             command,
                             argument,
                             app_response_for(command),
                             false,
                             false,
                             false,
                             false,
                             false,
                             0,
                             false,
                             out_response);
        if (last == Status::Ok) {
            return Status::Ok;
        }
    }
    return last;
}

bool init_mmc_card(DeviceState& device) {
    ControllerState& controller = *device.controller;
    uint32_t ocr = 0;
    bool ready = false;
    for (uint32_t attempt = 0; attempt < kInitRetries; ++attempt) {
        Status status = command_with_retries(controller,
                                            CmdSendOpCond,
                                            OcrVoltageMask | OcrSectorMode,
                                            false,
                                            false,
                                            &ocr);
        if (status == Status::Ok && (ocr & OcrBusy) != 0) {
            ready = true;
            break;
        }
        for (uint32_t delay = 0; delay < 10000; ++delay) {
            cpu_relax();
        }
    }
    if (!ready) {
        log_message(LogLevel::Warn,
                    "sdhci: eMMC did not become ready last_ocr=%08x present=%08x normal=%04x error=%04x",
                    static_cast<unsigned int>(ocr),
                    static_cast<unsigned int>(read32(controller, RegPresentState)),
                    static_cast<unsigned int>(read16(controller, RegNormalIntStatus)),
                    static_cast<unsigned int>(read16(controller, RegErrorIntStatus)));
        return false;
    }
    device.sector_addressed = (ocr & OcrSectorMode) != 0;
    device.kind = CardKind::Mmc;

    if (command_with_retries(controller, CmdAllSendCid, 0, false, false) !=
        Status::Ok) {
        return false;
    }

    device.rca = kDefaultRca;
    if (command_with_retries(controller,
                             CmdSetRelativeAddr,
                             static_cast<uint32_t>(device.rca) << 16,
                             false,
                             false) != Status::Ok) {
        return false;
    }

    (void)command_with_retries(controller,
                               CmdSendCsd,
                               static_cast<uint32_t>(device.rca) << 16,
                               false,
                               false);

    if (command_with_retries(controller,
                             CmdSelectCard,
                             static_cast<uint32_t>(device.rca) << 16,
                             false,
                             false) != Status::Ok) {
        return false;
    }

    if (!set_clock(controller, 25000000)) {
        log_message(LogLevel::Warn, "sdhci: high-speed clock setup failed");
    }

    write16(controller, RegBlockSize, 512);
    write16(controller, RegBlockCount, 1);
    memset(g_ext_csd, 0, sizeof(g_ext_csd));
    if (command_with_retries(controller,
                             CmdSendExtCsd,
                             0,
                             true,
                             true) != Status::Ok ||
        read_data_block(controller, g_ext_csd) != Status::Ok) {
        return false;
    }

    uint32_t sectors = read_u32_le(g_ext_csd + 212);
    if (sectors == 0) {
        return false;
    }

    if (!enable_8bit_bus(controller)) {
        (void)enable_4bit_bus(controller);
    }
    (void)enable_high_speed(controller);
    init_adma2(device);

    copy_printable(device.identify.model,
                   sizeof(device.identify.model),
                   g_ext_csd + 136,
                   16);
    if (device.identify.model[0] == '\0') {
        copy_printable(device.identify.model,
                       sizeof(device.identify.model),
                       g_ext_csd + 254,
                       6);
    }
    device.identify.sector_count = sectors;
    device.identify.removable = false;
    device.identify.present = true;
    device.present = true;
    return true;
}

bool init_sd_card(DeviceState& device) {
    ControllerState& controller = *device.controller;
    uint32_t response = 0;
    bool version_2 = false;
    Status check = command_with_retries(controller,
                                        CmdSendExtCsd,
                                        SdVoltageAccepted | SdCheckPattern,
                                        false,
                                        false,
                                        &response);
    if (check == Status::Ok &&
        (response & 0xFFFu) == (SdVoltageAccepted | SdCheckPattern)) {
        version_2 = true;
    }

    uint32_t ocr = 0;
    bool ready = false;
    for (uint32_t attempt = 0; attempt < kInitRetries; ++attempt) {
        uint32_t arg = OcrVoltageMask;
        if (version_2) {
            arg |= SdOcrHighCapacity;
        }
        Status status = app_command(controller, 0, AcmdSdSendOpCond, arg, &ocr);
        if (status == Status::Ok && (ocr & OcrBusy) != 0) {
            ready = true;
            break;
        }
        if (status != Status::Ok && attempt == 0) {
            return false;
        }
        for (uint32_t delay = 0; delay < 10000; ++delay) {
            cpu_relax();
        }
    }
    if (!ready) {
        return false;
    }

    device.kind = CardKind::Sd;
    device.sector_addressed = (ocr & OcrSectorMode) != 0;

    if (command_with_retries(controller, CmdAllSendCid, 0, false, false) !=
        Status::Ok) {
        return false;
    }

    if (command_with_retries(controller,
                             CmdSetRelativeAddr,
                             0,
                             false,
                             false,
                             &response) != Status::Ok) {
        return false;
    }
    device.rca = static_cast<uint16_t>(response >> 16);
    if (device.rca == 0) {
        return false;
    }

    uint8_t csd[16]{};
    if (command_with_retries(controller,
                             CmdSendCsd,
                             static_cast<uint32_t>(device.rca) << 16,
                             false,
                             false) != Status::Ok) {
        return false;
    }
    read_response_136(controller, csd);
    uint64_t sectors = sectors_from_sd_csd(csd);
    if (sectors == 0) {
        return false;
    }

    if (command_with_retries(controller,
                             CmdSelectCard,
                             static_cast<uint32_t>(device.rca) << 16,
                             false,
                             false) != Status::Ok) {
        return false;
    }

    if (!device.sector_addressed &&
        command_with_retries(controller,
                             CmdSetBlockLength,
                             512,
                             false,
                             false) != Status::Ok) {
        return false;
    }

    if (app_command(controller, device.rca, AcmdSetBusWidth, 2) == Status::Ok) {
        write8(controller,
               RegHostControl1,
               static_cast<uint8_t>((read8(controller, RegHostControl1) &
                                     ~HostControl1EightBit) |
                                    HostControl1FourBit));
    }
    if (!set_clock(controller, 25000000)) {
        log_message(LogLevel::Warn, "sdhci: SD default-speed clock setup failed");
    }
    init_adma2(device);

    copy_printable(device.identify.model,
                   sizeof(device.identify.model),
                   reinterpret_cast<const uint8_t*>("SD_CARD"),
                   7);
    device.identify.sector_count = sectors;
    device.identify.removable = true;
    device.identify.present = true;
    device.present = true;
    return true;
}

bool init_card(DeviceState& device) {
    if (prepare_card(*device.controller) && init_sd_card(device)) {
        return true;
    }
    device.adma2_supported = false;
    device.adma2_64bit = false;
    device.adma2_failed = false;
    device.auto_cmd23_failed = false;
    if (prepare_card(*device.controller) && init_mmc_card(device)) {
        return true;
    }
    return false;
}

void format_device_name(char* buffer,
                        size_t buffer_size,
                        const char* prefix,
                        size_t index) {
    if (buffer == nullptr || buffer_size == 0) {
        return;
    }
    size_t pos = 0;
    while (prefix[pos] != '\0' && pos + 1 < buffer_size) {
        buffer[pos] = prefix[pos];
        ++pos;
    }

    char digits[10];
    size_t count = 0;
    do {
        digits[count++] = static_cast<char>('0' + (index % 10));
        index /= 10;
    } while (index > 0 && count < sizeof(digits));

    for (size_t i = 0; i < count && pos + 1 < buffer_size; ++i) {
        buffer[pos++] = digits[count - 1 - i];
    }
    buffer[pos] = '\0';
}

void enable_pci_device(const pci::PciDevice& device) {
    uint16_t command = pci::read_config16(device, 0x04);
    command |= static_cast<uint16_t>(kPciCommandMemorySpace |
                                     kPciCommandBusMaster);
    pci::write_config16(device, 0x04, command);
}

void probe_controller(const pci::PciDevice& pci_device) {
    if (g_device_count >= kMaxDevices) {
        return;
    }

    ControllerState* controller = nullptr;
    for (auto& candidate : g_controllers) {
        if (!candidate.used) {
            controller = &candidate;
            break;
        }
    }
    if (controller == nullptr) {
        return;
    }

    enable_pci_device(pci_device);
    uint8_t bar_index = 0;
    uint64_t bar = find_mmio_bar(pci_device, bar_index);
    if (bar == 0) {
        log_message(LogLevel::Warn,
                    "sdhci: missing MMIO BAR on %02u:%02u.%u",
                    pci_device.bus,
                    pci_device.slot,
                    pci_device.function);
        return;
    }

    volatile uint8_t* regs = map_mmio_range(bar, kRegisterMapLength);
    if (regs == nullptr) {
        return;
    }

    controller->used = true;
    controller->pci_device = pci_device;
    controller->regs = regs;
    controller->regs_phys = bar;
    controller->capabilities = read32(*controller, RegCapabilities);
    controller->capabilities |=
        static_cast<uint64_t>(read32(*controller, RegCapabilities + 4)) << 32;

    log_message(LogLevel::Info,
                "sdhci: mapped BAR%u phys=%016llx virt=%016llx",
                static_cast<unsigned int>(bar_index),
                static_cast<unsigned long long>(bar),
                reinterpret_cast<unsigned long long>(regs));

    DeviceState& device = g_devices[g_device_count];
    device = {};
    device.used = true;
    device.controller = controller;

    if (!init_card(device)) {
        log_message(LogLevel::Warn,
                    "sdhci: no readable SD/eMMC card on %02u:%02u.%u",
                    pci_device.bus,
                    pci_device.slot,
                    pci_device.function);
        device = {};
        return;
    }

    format_device_name(device.name,
                       sizeof(device.name),
                       device.identify.removable ? "SD_" : "EMMC_",
                       g_device_count);
    log_message(LogLevel::Info,
                "sdhci: registered %s model='%s' sectors=%llu",
                device.name,
                device.identify.model,
                static_cast<unsigned long long>(device.identify.sector_count));
    ++g_device_count;
}

bool is_supported_controller(const pci::PciDevice& device) {
    if (device.vendor == 0x8086 && device.device == 0x31CC) {
        return true;
    }
    return device.class_code == 0x08 && device.subclass == 0x05;
}

}  // namespace

bool init() {
    if (g_initialized) {
        return g_device_count > 0;
    }
    g_initialized = true;
    g_device_count = 0;

    const pci::PciDevice* list = pci::devices();
    size_t count = pci::device_count();
    size_t matched = 0;
    for (size_t i = 0; i < count; ++i) {
        if (!is_supported_controller(list[i])) {
            continue;
        }
        ++matched;
        log_message(LogLevel::Info,
                    "sdhci: probing %02u:%02u.%u vendor=%04x device=%04x",
                    list[i].bus,
                    list[i].slot,
                    list[i].function,
                    list[i].vendor,
                    list[i].device);
        probe_controller(list[i]);
    }

    if (matched == 0) {
        log_message(LogLevel::Info, "sdhci: no supported PCI SD host controllers found");
    } else if (g_device_count == 0) {
        log_message(LogLevel::Warn,
                    "sdhci: %zu controller(s) matched but no SD/eMMC devices initialized",
                    matched);
    }

    return g_device_count > 0;
}

size_t device_count() {
    init();
    return g_device_count;
}

const IdentifyInfo& identify(size_t device_index) {
    init();
    if (device_index >= g_device_count || !g_devices[device_index].present) {
        return kEmptyIdentifyInfo;
    }
    return g_devices[device_index].identify;
}

const char* device_name(size_t device_index) {
    init();
    if (device_index >= g_device_count || !g_devices[device_index].present) {
        return "SDHCI_INVALID";
    }
    return g_devices[device_index].name;
}

Status read_sectors(size_t device_index,
                    uint64_t lba,
                    uint8_t sector_count,
                    void* buffer) {
    init();
    if (device_index >= g_device_count || buffer == nullptr || sector_count == 0) {
        return Status::NoDevice;
    }

    DeviceState& device = g_devices[device_index];
    if (!device.present || device.controller == nullptr) {
        return Status::NoDevice;
    }

    uint64_t last_lba = lba + static_cast<uint64_t>(sector_count);
    if (lba >= device.identify.sector_count ||
        last_lba > device.identify.sector_count) {
        return Status::IoError;
    }

    auto* out = static_cast<uint8_t*>(buffer);
    lock_device(device);
    Status result = Status::Ok;
    uint32_t argument = device.sector_addressed
                            ? static_cast<uint32_t>(lba)
                            : static_cast<uint32_t>(lba * 512ull);
    write16(*device.controller, RegBlockSize, 512);
    write16(*device.controller, RegBlockCount, sector_count);
    if (device.adma2_supported && !device.adma2_failed) {
        result = Status::IoError;
        bool is_sd_multi = device.kind == CardKind::Sd && sector_count > 1;
        bool try_auto_cmd23 = device.kind == CardKind::Mmc && sector_count > 1 &&
                              !device.auto_cmd23_failed &&
                              supports_auto_cmd23(*device.controller);
        if (is_sd_multi) {
            result = transfer_data_dma(device,
                                       CmdReadMultipleBlock,
                                       argument,
                                       true,
                                       true,
                                       out,
                                       sector_count,
                                       false,
                                       true);
            if (result == Status::Ok) {
                unlock_device(device);
                return Status::Ok;
            }
        } else if (try_auto_cmd23) {
            result = transfer_data_dma(device,
                                       CmdReadMultipleBlock,
                                       argument,
                                       true,
                                       true,
                                       out,
                                       sector_count,
                                       true,
                                       false);
            if (result == Status::Ok) {
                unlock_device(device);
                return Status::Ok;
            }
        }
        if (device.kind == CardKind::Mmc && sector_count > 1) {
            write16(*device.controller, RegBlockCount, sector_count);
            if (set_block_count(*device.controller, sector_count) == Status::Ok) {
                result = transfer_data_dma(device,
                                           CmdReadMultipleBlock,
                                           argument,
                                           true,
                                           true,
                                           out,
                                           sector_count,
                                           false,
                                           false);
            }
        } else if (sector_count == 1) {
            result = transfer_data_dma(device,
                                       CmdReadSingleBlock,
                                       argument,
                                       true,
                                       false,
                                       out,
                                       sector_count,
                                       false,
                                       false);
        }
        if (result == Status::Ok) {
            unlock_device(device);
            return Status::Ok;
        }
    }

    if (sector_count == 1) {
        result = command_with_retries(*device.controller,
                                      CmdReadSingleBlock,
                                      argument,
                                      true,
                                      true);
        if (result == Status::Ok) {
            result = read_data_block(*device.controller, out);
        }
    } else {
        bool is_sd_multi = device.kind == CardKind::Sd;
        bool try_auto_cmd23 = device.kind == CardKind::Mmc &&
                              !device.auto_cmd23_failed &&
                              supports_auto_cmd23(*device.controller);
        if (is_sd_multi || try_auto_cmd23) {
            result = transfer_data_pio(*device.controller,
                                       CmdReadMultipleBlock,
                                       argument,
                                       true,
                                       true,
                                       out,
                                       sector_count,
                                       try_auto_cmd23,
                                       is_sd_multi);
            if (try_auto_cmd23 && result != Status::Ok) {
                device.auto_cmd23_failed = true;
            }
        }
        if (!is_sd_multi && (!try_auto_cmd23 || result != Status::Ok)) {
            write16(*device.controller, RegBlockCount, sector_count);
            result = set_block_count(*device.controller, sector_count);
            if (result == Status::Ok) {
                result = transfer_data_pio(*device.controller,
                                           CmdReadMultipleBlock,
                                           argument,
                                           true,
                                           true,
                                           out,
                                           sector_count,
                                           false,
                                           false);
            }
        }
    }
    unlock_device(device);
    return result;
}

Status write_sectors(size_t device_index,
                     uint64_t lba,
                     uint8_t sector_count,
                     const void* buffer) {
    init();
    if (device_index >= g_device_count || buffer == nullptr || sector_count == 0) {
        return Status::NoDevice;
    }

    DeviceState& device = g_devices[device_index];
    if (!device.present || device.controller == nullptr) {
        return Status::NoDevice;
    }

    uint64_t last_lba = lba + static_cast<uint64_t>(sector_count);
    if (lba >= device.identify.sector_count ||
        last_lba > device.identify.sector_count) {
        return Status::IoError;
    }

    auto* in = static_cast<const uint8_t*>(buffer);
    lock_device(device);
    Status result = Status::Ok;
    uint32_t argument = device.sector_addressed
                            ? static_cast<uint32_t>(lba)
                            : static_cast<uint32_t>(lba * 512ull);
    write16(*device.controller, RegBlockSize, 512);
    write16(*device.controller, RegBlockCount, sector_count);
    if (device.adma2_supported && !device.adma2_failed) {
        result = Status::IoError;
        bool is_sd_multi = device.kind == CardKind::Sd && sector_count > 1;
        bool try_auto_cmd23 = device.kind == CardKind::Mmc && sector_count > 1 &&
                              !device.auto_cmd23_failed &&
                              supports_auto_cmd23(*device.controller);
        if (is_sd_multi) {
            result = transfer_data_dma(device,
                                       CmdWriteMultipleBlock,
                                       argument,
                                       false,
                                       true,
                                       const_cast<uint8_t*>(in),
                                       sector_count,
                                       false,
                                       true);
            if (result == Status::Ok) {
                unlock_device(device);
                return Status::Ok;
            }
        } else if (try_auto_cmd23) {
            result = transfer_data_dma(device,
                                       CmdWriteMultipleBlock,
                                       argument,
                                       false,
                                       true,
                                       const_cast<uint8_t*>(in),
                                       sector_count,
                                       true,
                                       false);
            if (result == Status::Ok) {
                unlock_device(device);
                return Status::Ok;
            }
        }
        if (device.kind == CardKind::Mmc && sector_count > 1) {
            write16(*device.controller, RegBlockCount, sector_count);
            if (set_block_count(*device.controller, sector_count) == Status::Ok) {
                result = transfer_data_dma(device,
                                           CmdWriteMultipleBlock,
                                           argument,
                                           false,
                                           true,
                                           const_cast<uint8_t*>(in),
                                           sector_count,
                                           false,
                                           false);
            }
        } else if (sector_count == 1) {
            result = transfer_data_dma(device,
                                       CmdWriteSingleBlock,
                                       argument,
                                       false,
                                       false,
                                       const_cast<uint8_t*>(in),
                                       sector_count,
                                       false,
                                       false);
        }
        if (result == Status::Ok) {
            unlock_device(device);
            return Status::Ok;
        }
    }

    if (sector_count == 1) {
        result = command_with_retries(*device.controller,
                                      CmdWriteSingleBlock,
                                      argument,
                                      true,
                                      false);
        if (result == Status::Ok) {
            result = write_data_block(*device.controller, in);
        }
    } else {
        bool is_sd_multi = device.kind == CardKind::Sd;
        bool try_auto_cmd23 = device.kind == CardKind::Mmc &&
                              !device.auto_cmd23_failed &&
                              supports_auto_cmd23(*device.controller);
        if (is_sd_multi || try_auto_cmd23) {
            result = transfer_data_pio(*device.controller,
                                       CmdWriteMultipleBlock,
                                       argument,
                                       false,
                                       true,
                                       const_cast<uint8_t*>(in),
                                       sector_count,
                                       try_auto_cmd23,
                                       is_sd_multi);
            if (try_auto_cmd23 && result != Status::Ok) {
                device.auto_cmd23_failed = true;
            }
        }
        if (!is_sd_multi && (!try_auto_cmd23 || result != Status::Ok)) {
            write16(*device.controller, RegBlockCount, sector_count);
            result = set_block_count(*device.controller, sector_count);
            if (result == Status::Ok) {
                result = transfer_data_pio(*device.controller,
                                           CmdWriteMultipleBlock,
                                           argument,
                                           false,
                                           true,
                                           const_cast<uint8_t*>(in),
                                           sector_count,
                                           false,
                                           false);
            }
        }
    }
    unlock_device(device);
    return result;
}

}  // namespace sdhci
