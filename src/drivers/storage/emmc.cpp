#include "drivers/storage/emmc.hpp"

#include <stddef.h>
#include <stdint.h>

#include "drivers/limine/limine_requests.hpp"
#include "drivers/log/logging.hpp"
#include "drivers/pci/pci.hpp"
#include "lib/mem.hpp"
#include "arch/x86_64/memory/paging.hpp"

namespace {

constexpr size_t kMaxControllers = 4;
constexpr uint32_t kDefaultBlockSize = 512;
constexpr uint32_t kInitClockHz = 400000;
constexpr uint32_t kTransferClockHz = 25000000;
constexpr uint32_t kCmdRetryCount = 1000;
constexpr uint64_t kPageSize = 0x1000;

enum class ResponseType {
    None,
    Short,
    ShortBusy,
    Long,
};

struct Controller {
    pci::PciDevice pci;
    volatile uint8_t* regs;
    volatile uint8_t* vendor_regs;
    size_t vendor_size;
    uint32_t base_clock_hz;
    bool ready;
    bool gemini_lake;
};

struct Device {
    Controller* controller;
    uint32_t rca;
    uint64_t sector_count;
    bool initialized;
};

Controller g_controllers[kMaxControllers];
size_t g_controller_count = 0;

Device g_devices[kMaxControllers];
size_t g_device_count = 0;

bool g_initialized = false;

constexpr uint32_t SDHCI_DMA_ADDRESS = 0x00;
constexpr uint32_t SDHCI_BLOCK_SIZE = 0x04;
constexpr uint32_t SDHCI_BLOCK_COUNT = 0x06;
constexpr uint32_t SDHCI_ARGUMENT = 0x08;
constexpr uint32_t SDHCI_TRANSFER_MODE = 0x0C;
constexpr uint32_t SDHCI_COMMAND = 0x0E;
constexpr uint32_t SDHCI_RESPONSE0 = 0x10;
constexpr uint32_t SDHCI_RESPONSE1 = 0x14;
constexpr uint32_t SDHCI_RESPONSE2 = 0x18;
constexpr uint32_t SDHCI_RESPONSE3 = 0x1C;
constexpr uint32_t SDHCI_BUFFER_DATA_PORT = 0x20;
constexpr uint32_t SDHCI_PRESENT_STATE = 0x24;
constexpr uint32_t SDHCI_HOST_CONTROL = 0x28;
constexpr uint32_t SDHCI_POWER_CONTROL = 0x29;
constexpr uint32_t SDHCI_CLOCK_CONTROL = 0x2C;
constexpr uint32_t SDHCI_TIMEOUT_CONTROL = 0x2E;
constexpr uint32_t SDHCI_SOFTWARE_RESET = 0x2F;
constexpr uint32_t SDHCI_INT_STATUS = 0x30;
constexpr uint32_t SDHCI_INT_ENABLE = 0x34;
constexpr uint32_t SDHCI_SIGNAL_ENABLE = 0x38;
constexpr uint32_t SDHCI_CAPABILITIES = 0x40;
constexpr uint32_t SDHCI_HOST_CONTROL2 = 0x3E;

constexpr uint16_t SDHCI_CMD_RESP_NONE = 0x0000;
constexpr uint16_t SDHCI_CMD_RESP_LONG = 0x0001;
constexpr uint16_t SDHCI_CMD_RESP_SHORT = 0x0002;
constexpr uint16_t SDHCI_CMD_RESP_SHORT_BUSY = 0x0003;
constexpr uint16_t SDHCI_CMD_CRC = 1u << 3;
constexpr uint16_t SDHCI_CMD_INDEX_CHECK = 1u << 4;
constexpr uint16_t SDHCI_CMD_DATA_PRESENT = 1u << 5;
constexpr uint16_t SDHCI_CMD_TYPE_NORMAL = 0u << 6;
constexpr uint16_t SDHCI_CMD_INDEX_SHIFT = 8;

constexpr uint16_t SDHCI_CLOCK_INT_EN = 1u << 0;
constexpr uint16_t SDHCI_CLOCK_INT_STABLE = 1u << 1;
constexpr uint16_t SDHCI_CLOCK_CARD_EN = 1u << 2;
constexpr uint16_t SDHCI_DIVIDER_SHIFT = 8;
constexpr uint16_t SDHCI_DIVIDER_HI_SHIFT = 6;

constexpr uint16_t SDHCI_HOST_CTRL2_PRESET_ENABLE = 1u << 15;

constexpr uint32_t SDHCI_INT_CMD_COMPLETE = 1u << 0;
constexpr uint32_t SDHCI_INT_TRANSFER_COMPLETE = 1u << 1;
constexpr uint32_t SDHCI_INT_DMA = 1u << 3;
constexpr uint32_t SDHCI_INT_BUFFER_WRITE_READY = 1u << 4;
constexpr uint32_t SDHCI_INT_BUFFER_READ_READY = 1u << 5;
constexpr uint32_t SDHCI_INT_ERROR = 1u << 15;

constexpr uint32_t SDHCI_PRESENT_INHIBIT_CMD = 1u << 0;
constexpr uint32_t SDHCI_PRESENT_INHIBIT_DATA = 1u << 1;

uint64_t hhdm_offset() {
    if (hhdm_request.response != nullptr) {
        return hhdm_request.response->offset;
    }
    return 0;
}

uint8_t read8(const Controller& ctrl, uint32_t offset) {
    return *(ctrl.regs + offset);
}

uint16_t read16(const Controller& ctrl, uint32_t offset) {
    return *reinterpret_cast<volatile uint16_t*>(ctrl.regs + offset);
}

uint32_t read32(const Controller& ctrl, uint32_t offset) {
    return *reinterpret_cast<volatile uint32_t*>(ctrl.regs + offset);
}

void write8(const Controller& ctrl, uint32_t offset, uint8_t value) {
    *(ctrl.regs + offset) = value;
}

void write16(const Controller& ctrl, uint32_t offset, uint16_t value) {
    *reinterpret_cast<volatile uint16_t*>(ctrl.regs + offset) = value;
}

void write32(const Controller& ctrl, uint32_t offset, uint32_t value) {
    *reinterpret_cast<volatile uint32_t*>(ctrl.regs + offset) = value;
}

uint8_t* map_mmio_region(uint64_t phys, size_t length, uint64_t hhdm_off) {
    if (length == 0) {
        length = kPageSize;
    }
    uint64_t start = phys & ~(kPageSize - 1);
    uint64_t end = (phys + length + kPageSize - 1) & ~(kPageSize - 1);
    for (uint64_t addr = start; addr < end; addr += kPageSize) {
        uint64_t virt = (hhdm_off != 0) ? (hhdm_off + addr) : addr;
        paging_map_page(virt, addr, PAGE_FLAG_WRITE | PAGE_FLAG_GLOBAL);
    }
    uint64_t base = (hhdm_off != 0) ? (hhdm_off + phys) : phys;
    return reinterpret_cast<uint8_t*>(base);
}

void udelay() {
    for (int i = 0; i < 1000; ++i) {
        asm volatile("pause");
    }
}

bool intel_enable_power(Controller& ctrl) {
    if (ctrl.vendor_regs == nullptr || ctrl.vendor_size < 0xA8) {
        return true;
    }
    auto regs = ctrl.vendor_regs;
    volatile uint32_t* pwr_ctrl = reinterpret_cast<volatile uint32_t*>(regs + 0xA0);
    volatile uint32_t* clk_ctrl = reinterpret_cast<volatile uint32_t*>(regs + 0xA4);

    *pwr_ctrl = 0x0;
    udelay();
    *clk_ctrl = 0x0;
    udelay();

    *pwr_ctrl = 0x1;
    udelay();
    *clk_ctrl = 0x1;
    udelay();

    log_message(LogLevel::Info,
                "eMMC: vendor power sequence applied (regs=0x%llx)",
                static_cast<unsigned long long>(
                    reinterpret_cast<uintptr_t>(regs)));
    return true;
}

bool wait_for_clear(const Controller& ctrl, uint32_t reg, uint32_t mask,
                    uint32_t timeout) {
    while (timeout-- > 0) {
        if ((read32(ctrl, reg) & mask) == 0) {
            return true;
        }
        udelay();
    }
    return false;
}

bool wait_for_bits(const Controller& ctrl, uint32_t reg, uint32_t mask,
                   uint32_t timeout) {
    while (timeout-- > 0) {
        if ((read32(ctrl, reg) & mask) == mask) {
            return true;
        }
        udelay();
    }
    return false;
}

bool wait_reset_clear(const Controller& ctrl, uint8_t mask) {
    uint32_t timeout = 10000;
    while (timeout-- > 0) {
        if ((read8(ctrl, SDHCI_SOFTWARE_RESET) & mask) == 0) {
            return true;
        }
        udelay();
    }
    return false;
}

bool reset_line(const Controller& ctrl, uint8_t mask) {
    write8(ctrl, SDHCI_SOFTWARE_RESET, mask);
    if (!wait_reset_clear(ctrl, mask)) {
        return false;
    }
    return true;
}

void enable_power(const Controller& ctrl);
bool set_clock(Controller& ctrl, uint32_t hz);
bool wait_cmd_ready(const Controller& ctrl);
bool wait_data_ready(const Controller& ctrl);
void clear_interrupts(const Controller& ctrl);
bool reset_controller(const Controller& ctrl);
bool send_command(const Controller& ctrl,
                  uint8_t index,
                  uint32_t argument,
                  ResponseType response,
                  bool data_present,
                  uint16_t transfer_mode,
                  uint32_t* out_response);

bool setup_identification_bus(Controller& ctrl) {
    intel_enable_power(ctrl);
    enable_power(ctrl);
    log_message(LogLevel::Info,
                "eMMC: powering controller %02x:%02x.%01x",
                ctrl.pci.bus,
                ctrl.pci.slot,
                ctrl.pci.function);

    // Ensure 1-bit bus width before negotiating with the card
    uint8_t hc = read8(ctrl, SDHCI_HOST_CONTROL);
    hc &= ~(0b11 << 1);
    write8(ctrl, SDHCI_HOST_CONTROL, hc);

    uint16_t host_ctrl2 = read16(ctrl, SDHCI_HOST_CONTROL2);
    host_ctrl2 &= ~SDHCI_HOST_CTRL2_PRESET_ENABLE;
    write16(ctrl, SDHCI_HOST_CONTROL2, host_ctrl2);

    if (!set_clock(ctrl, kInitClockHz)) {
        log_message(LogLevel::Warn,
                    "eMMC: failed to set identification clock on %02x:%02x.%01x",
                    ctrl.pci.bus,
                    ctrl.pci.slot,
                    ctrl.pci.function);
        return false;
    }
    log_message(LogLevel::Info,
                "eMMC: identification clock enabled on %02x:%02x.%01x",
                ctrl.pci.bus,
                ctrl.pci.slot,
                ctrl.pci.function);

    if (!wait_cmd_ready(ctrl)) {
        log_message(LogLevel::Warn,
                    "eMMC: command line stuck busy before identification on %02x:%02x.%01x",
                    ctrl.pci.bus,
                    ctrl.pci.slot,
                    ctrl.pci.function);
        return false;
    }
    if (!wait_data_ready(ctrl)) {
        log_message(LogLevel::Warn,
                    "eMMC: data line stuck busy before identification on %02x:%02x.%01x",
                    ctrl.pci.bus,
                    ctrl.pci.slot,
                    ctrl.pci.function);
        return false;
    }

    host_ctrl2 = read16(ctrl, SDHCI_HOST_CONTROL2);
    if (ctrl.gemini_lake) {
        host_ctrl2 &= ~SDHCI_HOST_CTRL2_PRESET_ENABLE;
        log_message(LogLevel::Info,
                    "eMMC: leaving presets disabled during identification on %02x:%02x.%01x",
                    ctrl.pci.bus,
                    ctrl.pci.slot,
                    ctrl.pci.function);
    } else {
        host_ctrl2 |= SDHCI_HOST_CTRL2_PRESET_ENABLE;
    }
    write16(ctrl, SDHCI_HOST_CONTROL2, host_ctrl2);

    write8(ctrl, SDHCI_HOST_CONTROL, 0);
    write8(ctrl, SDHCI_TIMEOUT_CONTROL, 0x0E);

    clear_interrupts(ctrl);
    write32(ctrl, SDHCI_INT_ENABLE, 0xFFFFFFFF);
    write32(ctrl, SDHCI_SIGNAL_ENABLE, 0);
    log_message(LogLevel::Info,
                "eMMC: identification bus ready on %02x:%02x.%01x",
                ctrl.pci.bus,
                ctrl.pci.slot,
                ctrl.pci.function);
    return true;
}

bool cold_reset_to_idle(Controller& ctrl) {
    log_message(LogLevel::Info,
                "eMMC: performing cold reset on %02x:%02x.%01x",
                ctrl.pci.bus,
                ctrl.pci.slot,
                ctrl.pci.function);
    if (!reset_controller(ctrl)) {
        return false;
    }
    if (!setup_identification_bus(ctrl)) {
        return false;
    }
    if (!send_command(ctrl, 0, 0, ResponseType::None, false, 0, nullptr)) {
        log_message(LogLevel::Warn,
                    "eMMC: CMD0 failed on %02x:%02x.%01x",
                    ctrl.pci.bus,
                    ctrl.pci.slot,
                    ctrl.pci.function);
        return false;
    }
    int wait_loops = ctrl.gemini_lake ? 1500000 : 500000;
    for (int i = 0; i < wait_loops; ++i) {
        asm volatile("pause");
    }
    return true;
}

void enable_presets_for_transfer(Controller& ctrl) {
    if (!ctrl.gemini_lake) {
        return;
    }
    uint16_t host_ctrl2 = read16(ctrl, SDHCI_HOST_CONTROL2);
    if ((host_ctrl2 & SDHCI_HOST_CTRL2_PRESET_ENABLE) != 0) {
        return;
    }
    host_ctrl2 |= SDHCI_HOST_CTRL2_PRESET_ENABLE;
    write16(ctrl, SDHCI_HOST_CONTROL2, host_ctrl2);
    log_message(LogLevel::Info,
                "eMMC: re-enabled presets for transfer on %02x:%02x.%01x",
                ctrl.pci.bus,
                ctrl.pci.slot,
                ctrl.pci.function);
}

bool reset_controller(const Controller& ctrl) {
    log_message(LogLevel::Info,
                "eMMC: resetting controller %02x:%02x.%01x",
                ctrl.pci.bus,
                ctrl.pci.slot,
                ctrl.pci.function);
    write8(ctrl, SDHCI_SOFTWARE_RESET, 0x07);
    if (!wait_reset_clear(ctrl, 0x07)) {
        log_message(LogLevel::Warn,
                    "eMMC: full reset timed out on %02x:%02x.%01x",
                    ctrl.pci.bus,
                    ctrl.pci.slot,
                    ctrl.pci.function);
        return false;
    }
    write8(ctrl, SDHCI_SOFTWARE_RESET, 0x01);
    if (!wait_reset_clear(ctrl, 0x01)) {
        log_message(LogLevel::Warn,
                    "eMMC: CMD line reset timed out on %02x:%02x.%01x",
                    ctrl.pci.bus,
                    ctrl.pci.slot,
                    ctrl.pci.function);
        return false;
    }
    return true;
}

uint16_t build_divisor(uint32_t base_clock, uint32_t target) {
    if (target == 0 || base_clock == 0) {
        return 0;
    }

    uint16_t divisor = 1;
    while ((base_clock / divisor) > target && divisor < 2048) {
        divisor <<= 1;
    }
    if (divisor > 2048) {
        divisor = 2048;
    }
    uint16_t encoded = 0;
    uint16_t lower = (divisor >> 1) & 0xFF;
    uint16_t upper = ((divisor >> 9) & 0x03);
    encoded |= static_cast<uint16_t>(lower) << SDHCI_DIVIDER_SHIFT;
    encoded |= static_cast<uint16_t>(upper) << SDHCI_DIVIDER_HI_SHIFT;
    return encoded;
}

bool set_clock(Controller& ctrl, uint32_t hz) {
    write16(ctrl, SDHCI_CLOCK_CONTROL, 0);
    if (hz == 0) {
        return true;
    }

    uint32_t base = ctrl.base_clock_hz ? ctrl.base_clock_hz : 50000000;
    uint16_t divisor = build_divisor(base, hz);

    uint16_t clk = divisor | SDHCI_CLOCK_INT_EN;
    write16(ctrl, SDHCI_CLOCK_CONTROL, clk);

    if (!wait_for_bits(ctrl,
                       SDHCI_CLOCK_CONTROL,
                       SDHCI_CLOCK_INT_STABLE,
                       10000)) {
        log_message(LogLevel::Warn,
                    "eMMC: controller %02x:%02x.%01x clock failed to stabilize for %u Hz",
                    ctrl.pci.bus,
                    ctrl.pci.slot,
                    ctrl.pci.function,
                    hz);
        return false;
    }

    clk |= SDHCI_CLOCK_CARD_EN;
    write16(ctrl, SDHCI_CLOCK_CONTROL, clk);

    return true;
}


void enable_power(const Controller& ctrl) {
    write8(ctrl, SDHCI_POWER_CONTROL, 0);
    for (int i = 0; i < 1000; i++) asm volatile("pause");

    write8(ctrl, SDHCI_POWER_CONTROL, 0x0E | 0x01);

    for (int i = 0; i < 100000; i++) asm volatile("pause");
}


void clear_interrupts(const Controller& ctrl) {
    write32(ctrl, SDHCI_INT_STATUS, 0xFFFFFFFF);
}

bool wait_for_interrupt(const Controller& ctrl, uint32_t mask,
                        uint32_t timeout) {
    while (timeout-- > 0) {
        uint32_t status = read32(ctrl, SDHCI_INT_STATUS);
        if (status & SDHCI_INT_ERROR) {
            write32(ctrl, SDHCI_INT_STATUS, status);
            return false;
        }
        if ((status & mask) != 0) {
            return true;
        }
        udelay();
    }
    return false;
}

bool wait_cmd_ready(const Controller& ctrl) {
    bool ready = wait_for_clear(ctrl,
                                SDHCI_PRESENT_STATE,
                                SDHCI_PRESENT_INHIBIT_CMD,
                                10000);
    if (!ready) {
        log_message(LogLevel::Warn,
                    "eMMC: CMD line stuck busy on %02x:%02x.%01x (present=%08x)",
                    ctrl.pci.bus,
                    ctrl.pci.slot,
                    ctrl.pci.function,
                    read32(ctrl, SDHCI_PRESENT_STATE));
    }
    return ready;
}

bool wait_data_ready(const Controller& ctrl) {
    bool ready = wait_for_clear(ctrl,
                                SDHCI_PRESENT_STATE,
                                SDHCI_PRESENT_INHIBIT_DATA,
                                10000);
    if (!ready) {
        log_message(LogLevel::Warn,
                    "eMMC: DATA line stuck busy on %02x:%02x.%01x (present=%08x)",
                    ctrl.pci.bus,
                    ctrl.pci.slot,
                    ctrl.pci.function,
                    read32(ctrl, SDHCI_PRESENT_STATE));
    }
    return ready;
}

uint16_t response_flags(ResponseType type) {
    switch (type) {
        case ResponseType::None:
            return SDHCI_CMD_RESP_NONE;
        case ResponseType::Short:
            return SDHCI_CMD_RESP_SHORT | SDHCI_CMD_CRC | SDHCI_CMD_INDEX_CHECK;
        case ResponseType::ShortBusy:
            return SDHCI_CMD_RESP_SHORT_BUSY | SDHCI_CMD_CRC |
                   SDHCI_CMD_INDEX_CHECK;
        case ResponseType::Long:
            return SDHCI_CMD_RESP_LONG | SDHCI_CMD_CRC | SDHCI_CMD_INDEX_CHECK;
        default:
            return SDHCI_CMD_RESP_NONE;
    }
}

bool send_command(const Controller& ctrl,
                  uint8_t index,
                  uint32_t argument,
                  ResponseType response,
                  bool data_present,
                  uint16_t transfer_mode,
                  uint32_t* out_response) {
    if (!wait_cmd_ready(ctrl)) {
        log_message(LogLevel::Warn,
                    "eMMC: CMD%u busy state never cleared on %02x:%02x.%01x (ARG=%08x)",
                    static_cast<unsigned>(index),
                    ctrl.pci.bus,
                    ctrl.pci.slot,
                    ctrl.pci.function,
                    argument);
        if (!reset_line(ctrl, 0x01) || !wait_cmd_ready(ctrl)) {
            log_message(LogLevel::Warn,
                        "eMMC: CMD%u reset failed on %02x:%02x.%01x (ARG=%08x)",
                        static_cast<unsigned>(index),
                        ctrl.pci.bus,
                        ctrl.pci.slot,
                        ctrl.pci.function,
                        argument);
            return false;
        }
    }
    if (data_present && !wait_data_ready(ctrl)) {
        log_message(LogLevel::Warn,
                    "eMMC: CMD%u data line stuck busy on %02x:%02x.%01x (ARG=%08x)",
                    static_cast<unsigned>(index),
                    ctrl.pci.bus,
                    ctrl.pci.slot,
                    ctrl.pci.function,
                    argument);
        if (!reset_line(ctrl, 0x02) || !wait_data_ready(ctrl)) {
            log_message(LogLevel::Warn,
                        "eMMC: CMD%u data reset failed on %02x:%02x.%01x (ARG=%08x)",
                        static_cast<unsigned>(index),
                        ctrl.pci.bus,
                        ctrl.pci.slot,
                        ctrl.pci.function,
                        argument);
            return false;
        }
    }

    clear_interrupts(ctrl);

    if (data_present) {
        write16(ctrl, SDHCI_TRANSFER_MODE, transfer_mode);
    }

    write32(ctrl, SDHCI_ARGUMENT, argument);

    uint16_t cmd = response_flags(response) |
                   SDHCI_CMD_TYPE_NORMAL |
                   (static_cast<uint16_t>(index) << SDHCI_CMD_INDEX_SHIFT);

    if (data_present) {
        cmd |= SDHCI_CMD_DATA_PRESENT;
    }

    write16(ctrl, SDHCI_COMMAND, cmd);

    // SPECIAL CASE: CMD0 (GO_IDLE_STATE) – card reset
    if (index == 0) {
        constexpr int kWaitIterations = 200000;
        for (int i = 0; i < kWaitIterations; ++i) {
            asm volatile("pause");
        }
        if (!wait_cmd_ready(ctrl)) {
            log_message(LogLevel::Warn,
                        "eMMC: CMD0 inhibit never cleared on %02x:%02x.%01x",
                        ctrl.pci.bus,
                        ctrl.pci.slot,
                        ctrl.pci.function);
            if (!reset_line(ctrl, 0x01) || !wait_cmd_ready(ctrl)) {
                log_message(LogLevel::Warn,
                            "eMMC: CMD0 inhibit persists after reset on %02x:%02x.%01x",
                            ctrl.pci.bus,
                            ctrl.pci.slot,
                            ctrl.pci.function);
                return false;
            }
            log_message(LogLevel::Info,
                        "eMMC: CMD0 inhibit cleared after reset on %02x:%02x.%01x",
                        ctrl.pci.bus,
                        ctrl.pci.slot,
                        ctrl.pci.function);
        }
        return true;  // CMD0 never generates an interrupt
    }

    if (!wait_for_interrupt(ctrl, SDHCI_INT_CMD_COMPLETE, 100000)) {
        log_message(LogLevel::Warn,
                    "eMMC: command %u timed out on %02x:%02x.%01x (ARG=%08x)",
                    static_cast<unsigned>(index),
                    ctrl.pci.bus,
                    ctrl.pci.slot,
                    ctrl.pci.function,
                    argument);
        return false;
    }
    write32(ctrl, SDHCI_INT_STATUS, SDHCI_INT_CMD_COMPLETE);

    if (out_response != nullptr) {
        switch (response) {
            case ResponseType::Short:
            case ResponseType::ShortBusy:
                out_response[0] = read32(ctrl, SDHCI_RESPONSE0);
                break;
            case ResponseType::Long:
                out_response[0] = read32(ctrl, SDHCI_RESPONSE0);
                out_response[1] = read32(ctrl, SDHCI_RESPONSE1);
                out_response[2] = read32(ctrl, SDHCI_RESPONSE2);
                out_response[3] = read32(ctrl, SDHCI_RESPONSE3);
                break;
            default:
                break;
        }
    }

    return true;
}


bool read_data(const Controller& ctrl, void* buffer, size_t length) {
    uint8_t* out = static_cast<uint8_t*>(buffer);
    size_t remaining = length;

    while (remaining > 0) {
        size_t chunk = (remaining >= kDefaultBlockSize)
                           ? kDefaultBlockSize
                           : remaining;
        if (!wait_for_interrupt(ctrl, SDHCI_INT_BUFFER_READ_READY, 100000)) {
            log_message(LogLevel::Warn,
                        "eMMC: buffer read timeout while reading %zu bytes on %02x:%02x.%01x",
                        length,
                        ctrl.pci.bus,
                        ctrl.pci.slot,
                        ctrl.pci.function);
            return false;
        }
        for (size_t offset = 0; offset < chunk; offset += 4) {
            uint32_t value = read32(ctrl, SDHCI_BUFFER_DATA_PORT);
            size_t to_copy = (chunk - offset >= 4) ? 4 : (chunk - offset);
            for (size_t i = 0; i < to_copy; ++i) {
                out[offset + i] =
                    static_cast<uint8_t>((value >> (i * 8)) & 0xFF);
            }
        }
        out += chunk;
        remaining -= chunk;
        write32(ctrl, SDHCI_INT_STATUS, SDHCI_INT_BUFFER_READ_READY);
    }

    if (!wait_for_interrupt(ctrl, SDHCI_INT_TRANSFER_COMPLETE, 100000)) {
        log_message(LogLevel::Warn,
                    "eMMC: transfer completion timeout while reading %zu bytes on %02x:%02x.%01x",
                    length,
                    ctrl.pci.bus,
                    ctrl.pci.slot,
                    ctrl.pci.function);
        return false;
    }
    write32(ctrl, SDHCI_INT_STATUS, SDHCI_INT_TRANSFER_COMPLETE);
    return true;
}

bool read_single_block(const Controller& ctrl,
                       uint32_t argument,
                       void* buffer) {
    write16(ctrl, SDHCI_BLOCK_SIZE, static_cast<uint16_t>(kDefaultBlockSize));
    write16(ctrl, SDHCI_BLOCK_COUNT, 1);

    uint16_t transfer_mode = 0;
    transfer_mode |= (1u << 1);  // block count enable
    transfer_mode |= (1u << 3);  // read direction

    if (!send_command(ctrl, 17, argument, ResponseType::Short,
                      true, transfer_mode, nullptr)) {
        return false;
    }

    return read_data(ctrl, buffer, kDefaultBlockSize);
}

bool read_ext_csd(const Controller& ctrl, uint8_t* buffer) {
    write16(ctrl, SDHCI_BLOCK_SIZE, static_cast<uint16_t>(kDefaultBlockSize));
    write16(ctrl, SDHCI_BLOCK_COUNT, 1);
    uint16_t transfer_mode = 0;
    transfer_mode |= (1u << 1);
    transfer_mode |= (1u << 3);

    if (!send_command(ctrl, 8, 0, ResponseType::Short,
                      true, transfer_mode, nullptr)) {
        return false;
    }
    return read_data(ctrl, buffer, kDefaultBlockSize);
}

bool init_card(Controller& ctrl, Device& device) {
    uint32_t caps = read32(ctrl, SDHCI_CAPABILITIES);
    uint32_t base_clock = ((caps >> 8) & 0xFF) * 1000000;
    if (base_clock == 0) {
        base_clock = 50000000;
    }
    ctrl.base_clock_hz = base_clock;

    if (!cold_reset_to_idle(ctrl)) {
        return false;
    }

    uint32_t present = read32(ctrl, SDHCI_PRESENT_STATE);
    if ((present & (1u << 16)) == 0) {
        log_message(LogLevel::Warn,
                    "eMMC: controller %02x:%02x.%01x reports no card present",
                    ctrl.pci.bus, ctrl.pci.slot, ctrl.pci.function);
    }

    uint32_t ocr = 0x40FF8000;
    uint32_t response[4] = {};
    bool ready = false;
    for (uint32_t attempt = 0; attempt < kCmdRetryCount; ++attempt) {
        if (!send_command(ctrl, 1, ocr, ResponseType::Short, false, 0,
                          response)) {
            log_message(LogLevel::Warn,
                        "eMMC: CMD1 attempt %u failed, resetting",
                        attempt + 1);
            if (!cold_reset_to_idle(ctrl)) {
                return false;
            }
            continue;
        }

        if (response[0] & (1u << 31)) {
            ready = true;
            break;
        }
        int delay_iters = ctrl.gemini_lake ? 2000000 : 500000;
        for (int i = 0; i < delay_iters; ++i) {
            asm volatile("pause");
        }
    }
    if (!ready) {
        log_message(LogLevel::Warn,
                    "eMMC: device did not become ready");
        return false;
    }

    if (!send_command(ctrl, 2, 0, ResponseType::Long, false, 0, response)) {
        return false;
    }

    uint32_t rca = 1;
    if (!send_command(ctrl, 3, rca << 16, ResponseType::Short, false, 0,
                      response)) {
        return false;
    }

    if (!send_command(ctrl, 7, rca << 16, ResponseType::ShortBusy, false, 0,
                      nullptr)) {
        return false;
    }

    uint8_t ext_csd[512];
    memset(ext_csd, 0, sizeof(ext_csd));
    if (!read_ext_csd(ctrl, ext_csd)) {
        log_message(LogLevel::Warn, "eMMC: failed to read EXT_CSD");
        return false;
    } else {
        if (ext_csd[192] != 1) {
            log_message(LogLevel::Warn, "eMMC: unsupported EXT_CSD revision %u",
                        static_cast<unsigned>(ext_csd[192]));
            return false;
        } else {
            log_message(LogLevel::Info, "eMMC: EXT_CSD revision %u",
                        static_cast<unsigned>(ext_csd[192]));
        }
    }

    if (!send_command(ctrl, 16, kDefaultBlockSize, ResponseType::Short,
                      false, 0, nullptr)) {
        log_message(LogLevel::Warn, "eMMC: failed to set block length");
        return false;
    }

    if (!set_clock(ctrl, kTransferClockHz)) {
        log_message(LogLevel::Warn, "eMMC: unable to switch to transfer clock");
        return false;
    }
    enable_presets_for_transfer(ctrl);

    uint32_t sec_count = static_cast<uint32_t>(ext_csd[212]) |
                         (static_cast<uint32_t>(ext_csd[213]) << 8) |
                         (static_cast<uint32_t>(ext_csd[214]) << 16) |
                         (static_cast<uint32_t>(ext_csd[215]) << 24);
    if (sec_count == 0) {
        sec_count = 2048;
    }

    device.controller = &ctrl;
    device.rca = rca;
    device.sector_count = sec_count;
    device.initialized = true;
    return true;
}

bool read_mmio_bar(const pci::PciDevice& device,
                   uint8_t index,
                   uint64_t& base,
                   size_t& size,
                   uint8_t& next_index) {
    if (index >= 6) {
        return false;
    }
    uint8_t offset = static_cast<uint8_t>(0x10 + index * 4);
    uint32_t raw = pci::read_config32(device, offset);
    if (raw == 0 || raw == 0xFFFFFFFF) {
        next_index = index + 1;
        return false;
    }

    bool is_io_bar = (raw & 0x1) != 0;
    if (is_io_bar) {
        next_index = index + 1;
        return false;
    }

    bool is_64bit = (raw & 0x4) != 0;
    uint64_t address = raw & ~0xFULL;

    uint32_t original_low = raw;
    pci::write_config32(device, offset, 0xFFFFFFFF);
    uint32_t size_low = pci::read_config32(device, offset);
    pci::write_config32(device, offset, original_low);
    size_low &= ~0xFULL;

    uint64_t mask = size_low;
    if (is_64bit) {
        uint32_t upper = pci::read_config32(device, offset + 4);
        address |= (static_cast<uint64_t>(upper) << 32);

        uint32_t original_high = upper;
        pci::write_config32(device, offset + 4, 0xFFFFFFFF);
        uint32_t size_high = pci::read_config32(device, offset + 4);
        pci::write_config32(device, offset + 4, original_high);
        mask |= static_cast<uint64_t>(size_high) << 32;

        next_index = index + 2;
    } else {
        next_index = index + 1;
    }

    if (address == 0 || mask == 0 || mask == 0xFFFFFFFFFFFFFFFFull) {
        return false;
    }

    uint64_t region_size = (~mask + 1);
    if (region_size == 0) {
        return false;
    }

    base = address;
    size = static_cast<size_t>(region_size);
    return true;
}

}  // namespace

namespace emmc {

bool init() {
    if (g_initialized) {
        return g_device_count > 0;
    }
    g_initialized = true;

    const pci::PciDevice* devices = pci::devices();
    size_t total = pci::device_count();
    uint64_t offset = hhdm_offset();

    for (size_t i = 0; i < total && g_controller_count < kMaxControllers; ++i) {
        const auto& dev = devices[i];
        if (dev.class_code != 0x08 || dev.subclass != 0x05) {
            continue;
        }

        log_message(LogLevel::Info,
                    "eMMC: probing PCI %02x:%02x.%01x vendor=%04x device=%04x class=%02x/%02x prog=%02x",
                    dev.bus, dev.slot, dev.function,
                    dev.vendor, dev.device, dev.class_code,
                    dev.subclass, dev.prog_if);

        uint64_t bar0 = 0;
        size_t bar_size = 0;
        uint8_t next_index = 0;
        if (!read_mmio_bar(dev, 0, bar0, bar_size, next_index)) {
            log_message(LogLevel::Info,
                        "eMMC: %02x:%02x.%01x no usable MMIO BAR",
                        dev.bus, dev.slot, dev.function);
            continue;
        }
        if (bar_size == 0) {
            log_message(LogLevel::Info,
                        "eMMC: %02x:%02x.%01x BAR size is zero",
                        dev.bus, dev.slot, dev.function);
            continue;
        }

        uint16_t command = pci::read_config16(dev, 0x04);
        command |= 0x0001;  // I/O Space Enable (quirky Intel SDHCI needs this)
        command |= 0x0002;  // Memory Space Enable
        command |= 0x0004;  // Bus Master Enable
        pci::write_config16(dev, 0x04, command);

        Controller& ctrl = g_controllers[g_controller_count];
        ctrl.pci = dev;
        ctrl.vendor_regs = nullptr;
        ctrl.vendor_size = 0;
        ctrl.base_clock_hz = 0;
        ctrl.gemini_lake =
            (dev.vendor == 0x8086 &&
             (dev.device == 0x31cc || dev.device == 0x31d0));

        uint8_t* mapped = map_mmio_region(bar0, bar_size, offset);
        if (mapped == nullptr) {
            log_message(LogLevel::Warn,
                        "eMMC: %02x:%02x.%01x failed to map BAR (phys=%016llx size=%zu)",
                        dev.bus, dev.slot, dev.function,
                        static_cast<unsigned long long>(bar0), bar_size);
            continue;
        }
        ctrl.regs = mapped;
        ctrl.ready = false;

        log_message(LogLevel::Info,
                    "eMMC: %02x:%02x.%01x BAR0 phys=%016llx size=%zu mapped=%p (hhdm=%016llx)",
                    dev.bus, dev.slot, dev.function,
                    static_cast<unsigned long long>(bar0),
                    bar_size,
                    static_cast<const void*>(mapped),
                    static_cast<unsigned long long>(offset));

        uint64_t vendor_phys = 0;
        size_t vendor_size = 0;
        uint8_t vendor_next = next_index;
        if (read_mmio_bar(dev, next_index, vendor_phys, vendor_size, vendor_next) &&
            vendor_size != 0) {
            uint8_t* vendor_regs = map_mmio_region(vendor_phys, vendor_size, offset);
            if (vendor_regs != nullptr) {
                ctrl.vendor_regs = vendor_regs;
                ctrl.vendor_size = vendor_size;
                log_message(LogLevel::Info,
                            "eMMC: %02x:%02x.%01x vendor BAR phys=%016llx size=%zu mapped=%p",
                            dev.bus, dev.slot, dev.function,
                            static_cast<unsigned long long>(vendor_phys),
                            vendor_size,
                            static_cast<const void*>(vendor_regs));
            } else {
                log_message(LogLevel::Warn,
                            "eMMC: %02x:%02x.%01x failed to map vendor BAR (phys=%016llx size=%zu)",
                            dev.bus, dev.slot, dev.function,
                            static_cast<unsigned long long>(vendor_phys),
                            vendor_size);
            }
        }

        if (g_device_count >= kMaxControllers) {
            break;
        }

        Device& device = g_devices[g_device_count];
        device.initialized = false;
        if (init_card(ctrl, device)) {
            ++g_controller_count;
            ++g_device_count;
            log_message(LogLevel::Info,
                        "eMMC: initialized controller %02x:%02x.%01x (sectors=%llu)",
                        dev.bus, dev.slot, dev.function,
                        static_cast<unsigned long long>(device.sector_count));
        }
    }

    return g_device_count > 0;
}

size_t device_count() {
    return g_device_count;
}

uint64_t device_sector_count(size_t index) {
    if (index >= g_device_count) {
        return 0;
    }
    return g_devices[index].sector_count;
}

Status read_blocks(size_t index, uint32_t lba, uint8_t count, void* buffer) {
    if (index >= g_device_count || !g_devices[index].initialized) {
        return Status::NoDevice;
    }
    Device& device = g_devices[index];
    Controller& ctrl = *device.controller;

    uint8_t* out = static_cast<uint8_t*>(buffer);
    for (uint8_t block = 0; block < count; ++block) {
        uint32_t current_lba = lba + block;
        if (!read_single_block(ctrl, current_lba, out)) {
            log_message(LogLevel::Warn,
                        "eMMC: block read failed (LBA=%u count=%u) on %02x:%02x.%01x",
                        current_lba,
                        static_cast<unsigned>(count),
                        ctrl.pci.bus,
                        ctrl.pci.slot,
                        ctrl.pci.function);
            return Status::IoError;
        }
        out += kDefaultBlockSize;
    }
    return Status::Ok;
}

}  // namespace emmc
