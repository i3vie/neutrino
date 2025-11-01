#include "pci.hpp"

#include "arch/x86_64/io.hpp"
#include "drivers/log/logging.hpp"

namespace {

constexpr uint16_t kConfigAddressPort = 0xCF8;
constexpr uint16_t kConfigDataPort = 0xCFC;
constexpr size_t kMaxDeviceCount = 256;

pci::DeviceInfo g_devices[kMaxDeviceCount];
size_t g_device_count = 0;
bool g_initialized = false;

uint32_t build_config_address(const pci::DeviceAddress& address,
                              uint8_t offset) {
    uint32_t aligned_offset = static_cast<uint32_t>(offset & 0xFC);
    return 0x80000000u |
           (static_cast<uint32_t>(address.bus) << 16) |
           (static_cast<uint32_t>(address.device) << 11) |
           (static_cast<uint32_t>(address.function) << 8) |
           aligned_offset;
}

void register_device(const pci::DeviceInfo& info) {
    if (g_device_count >= kMaxDeviceCount) {
        log_message(LogLevel::Warn,
                    "PCI: device table full (capacity %zu)",
                    static_cast<size_t>(kMaxDeviceCount));
        return;
    }
    g_devices[g_device_count++] = info;
}

void enumerate_function(const pci::DeviceAddress& address) {
    uint16_t vendor_id = pci::read_config16(address, 0x00);
    if (vendor_id == 0xFFFF) {
        return;
    }

    uint16_t device_id = pci::read_config16(address, 0x02);
    uint32_t class_reg = pci::read_config32(address, 0x08);

    pci::DeviceInfo info{};
    info.address = address;
    info.vendor_id = vendor_id;
    info.device_id = device_id;
    info.class_code = static_cast<uint8_t>((class_reg >> 24) & 0xFF);
    info.subclass = static_cast<uint8_t>((class_reg >> 16) & 0xFF);
    info.prog_if = static_cast<uint8_t>((class_reg >> 8) & 0xFF);
    info.revision = static_cast<uint8_t>(class_reg & 0xFF);

    register_device(info);

    log_message(LogLevel::Info,
                "PCI: %02x:%02x.%u vendor=%04x device=%04x class=%02x.%02x.%02x rev=%02x",
                static_cast<unsigned int>(address.bus),
                static_cast<unsigned int>(address.device),
                static_cast<unsigned int>(address.function),
                static_cast<unsigned int>(info.vendor_id),
                static_cast<unsigned int>(info.device_id),
                static_cast<unsigned int>(info.class_code),
                static_cast<unsigned int>(info.subclass),
                static_cast<unsigned int>(info.prog_if),
                static_cast<unsigned int>(info.revision));
}

void enumerate_bus(uint8_t bus) {
    for (uint8_t device = 0; device < 32; ++device) {
        pci::DeviceAddress address{bus, device, 0};
        uint16_t vendor_id = pci::read_config16(address, 0x00);
        if (vendor_id == 0xFFFF) {
            continue;
        }

        enumerate_function(address);

        uint8_t header_type = pci::read_config8(address, 0x0E);
        if ((header_type & 0x80u) == 0) {
            continue;
        }

        for (uint8_t function = 1; function < 8; ++function) {
            pci::DeviceAddress func_address{bus, device, function};
            if (pci::read_config16(func_address, 0x00) == 0xFFFF) {
                continue;
            }
            enumerate_function(func_address);
        }
    }
}

void enumerate_all_buses() {
    for (uint16_t bus = 0; bus < 256; ++bus) {
        enumerate_bus(static_cast<uint8_t>(bus));
    }
}

}  // namespace

namespace pci {

void init() {
    if (g_initialized) {
        log_message(LogLevel::Warn, "PCI: init called more than once");
        return;
    }

    g_initialized = true;
    g_device_count = 0;

    log_message(LogLevel::Debug, "PCI: enumerating devices");
    enumerate_all_buses();
    log_message(LogLevel::Info, "PCI: found %zu device%s",
                g_device_count,
                (g_device_count == 1) ? "" : "s");
}

size_t device_count() {
    return g_device_count;
}

const DeviceInfo* devices() {
    return g_devices;
}

const DeviceInfo* find_device(uint16_t vendor_id, uint16_t device_id) {
    for (size_t i = 0; i < g_device_count; ++i) {
        if (g_devices[i].vendor_id == vendor_id &&
            g_devices[i].device_id == device_id) {
            return &g_devices[i];
        }
    }
    return nullptr;
}

const DeviceInfo* find_by_class(uint8_t class_code,
                                uint8_t subclass,
                                uint8_t prog_if) {
    for (size_t i = 0; i < g_device_count; ++i) {
        if (g_devices[i].class_code != class_code ||
            g_devices[i].subclass != subclass) {
            continue;
        }
        if (prog_if != 0xFF && g_devices[i].prog_if != prog_if) {
            continue;
        }
        return &g_devices[i];
    }
    return nullptr;
}

uint32_t read_config32(const DeviceAddress& address, uint8_t offset) {
    uint32_t config_address = build_config_address(address, offset);
    outl(kConfigAddressPort, config_address);
    return inl(kConfigDataPort);
}

uint16_t read_config16(const DeviceAddress& address, uint8_t offset) {
    uint32_t value = read_config32(address, offset);
    uint32_t shift = static_cast<uint32_t>(offset & 0x02u) * 8;
    return static_cast<uint16_t>((value >> shift) & 0xFFFFu);
}

uint8_t read_config8(const DeviceAddress& address, uint8_t offset) {
    uint32_t value = read_config32(address, offset);
    uint32_t shift = static_cast<uint32_t>(offset & 0x03u) * 8;
    return static_cast<uint8_t>((value >> shift) & 0xFFu);
}

}  // namespace pci
