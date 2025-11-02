#include "pci.hpp"

#include "arch/x86_64/io.hpp"
#include "drivers/log/logging.hpp"

namespace {

constexpr uint16_t kConfigAddressPort = 0xCF8;
constexpr uint16_t kConfigDataPort = 0xCFC;
constexpr size_t kMaxDeviceCount = 256;

pci::PciDevice g_devices[kMaxDeviceCount];
size_t g_device_count = 0;
bool g_initialized = false;

struct ProgIfDescriptor {
    uint8_t value;
    const char* name;
};

struct SubclassDescriptor {
    uint8_t value;
    const char* name;
    const ProgIfDescriptor* prog_ifs;
    size_t prog_if_count;
};

struct ClassDescriptor {
    uint8_t value;
    const char* name;
    const SubclassDescriptor* subclasses;
    size_t subclass_count;
};

constexpr ProgIfDescriptor kMassStorageSataProgIfs[] = {
    {0x00, "Vendor-specific SATA"},
    {0x01, "AHCI 1.0"},
    {0x02, "Serial Storage Bus"},
};

constexpr ProgIfDescriptor kSerialBusUsbProgIfs[] = {
    {0x00, "UHCI"},
    {0x10, "OHCI"},
    {0x20, "EHCI"},
    {0x30, "xHCI"},
    {0x80, "Unspecified"},
    {0xFE, "USB Device"},
};

constexpr SubclassDescriptor kClassUnclassifiedSubclasses[] = {
    {0x00, "Non-VGA compatible device", nullptr, 0},
    {0x01, "VGA compatible device", nullptr, 0},
    {0x80, "Other unclassified device", nullptr, 0},
};

constexpr SubclassDescriptor kClassMassStorageSubclasses[] = {
    {0x00, "SCSI bus controller", nullptr, 0},
    {0x01, "IDE controller", nullptr, 0},
    {0x02, "Floppy disk controller", nullptr, 0},
    {0x03, "IPI bus controller", nullptr, 0},
    {0x04, "RAID controller", nullptr, 0},
    {0x05, "ATA controller", nullptr, 0},
    {0x06, "Serial ATA controller", kMassStorageSataProgIfs,
     sizeof(kMassStorageSataProgIfs) / sizeof(kMassStorageSataProgIfs[0])},
    {0x07, "Serial Attached SCSI controller", nullptr, 0},
    {0x08, "Non-volatile memory controller", nullptr, 0},
    {0x80, "Other mass storage controller", nullptr, 0},
};

constexpr SubclassDescriptor kClassNetworkSubclasses[] = {
    {0x00, "Ethernet controller", nullptr, 0},
    {0x01, "Token Ring network controller", nullptr, 0},
    {0x02, "FDDI controller", nullptr, 0},
    {0x03, "ATM controller", nullptr, 0},
    {0x04, "ISDN controller", nullptr, 0},
    {0x05, "WorldFip controller", nullptr, 0},
    {0x06, "PICMG 2.14 multi computing", nullptr, 0},
    {0x07, "Infiniband controller", nullptr, 0},
    {0x08, "Fabric controller", nullptr, 0},
    {0x80, "Other network controller", nullptr, 0},
};

constexpr SubclassDescriptor kClassDisplaySubclasses[] = {
    {0x00, "VGA compatible controller", nullptr, 0},
    {0x01, "XGA controller", nullptr, 0},
    {0x02, "3D controller", nullptr, 0},
    {0x80, "Other display controller", nullptr, 0},
};

constexpr SubclassDescriptor kClassMultimediaSubclasses[] = {
    {0x00, "Multimedia video controller", nullptr, 0},
    {0x01, "Multimedia audio controller", nullptr, 0},
    {0x02, "Computer telephony device", nullptr, 0},
    {0x03, "Audio device", nullptr, 0},
    {0x80, "Other multimedia device", nullptr, 0},
};

constexpr SubclassDescriptor kClassMemorySubclasses[] = {
    {0x00, "RAM controller", nullptr, 0},
    {0x01, "Flash memory controller", nullptr, 0},
    {0x80, "Other memory controller", nullptr, 0},
};

constexpr SubclassDescriptor kClassBridgeSubclasses[] = {
    {0x00, "Host bridge", nullptr, 0},
    {0x01, "ISA bridge", nullptr, 0},
    {0x02, "EISA bridge", nullptr, 0},
    {0x03, "MicroChannel bridge", nullptr, 0},
    {0x04, "PCI-to-PCI bridge", nullptr, 0},
    {0x05, "PCMCIA bridge", nullptr, 0},
    {0x06, "NuBus bridge", nullptr, 0},
    {0x07, "CardBus bridge", nullptr, 0},
    {0x08, "RACEway bridge", nullptr, 0},
    {0x09, "PCI-to-PCI bridge (secondary)", nullptr, 0},
    {0x0A, "InfiniBand-to-PCI bridge", nullptr, 0},
    {0x80, "Other bridge device", nullptr, 0},
};

constexpr SubclassDescriptor kClassSimpleCommSubclasses[] = {
    {0x00, "Serial controller", nullptr, 0},
    {0x01, "Parallel controller", nullptr, 0},
    {0x02, "Multiport serial controller", nullptr, 0},
    {0x03, "Modem", nullptr, 0},
    {0x04, "IEEE 488.1/2 (GPIB) controller", nullptr, 0},
    {0x80, "Other communication controller", nullptr, 0},
};

constexpr SubclassDescriptor kClassBaseSystemSubclasses[] = {
    {0x00, "Programmable interrupt controller", nullptr, 0},
    {0x01, "DMA controller", nullptr, 0},
    {0x02, "Timer", nullptr, 0},
    {0x03, "RTC controller", nullptr, 0},
    {0x04, "PCI hot-plug controller", nullptr, 0},
    {0x05, "SD host controller", nullptr, 0},
    {0x06, "IOMMU", nullptr, 0},
    {0x80, "Other base system peripheral", nullptr, 0},
};

constexpr SubclassDescriptor kClassInputDeviceSubclasses[] = {
    {0x00, "Keyboard controller", nullptr, 0},
    {0x01, "Digitizer", nullptr, 0},
    {0x02, "Mouse controller", nullptr, 0},
    {0x03, "Scanner controller", nullptr, 0},
    {0x04, "Gameport controller", nullptr, 0},
    {0x80, "Other input device", nullptr, 0},
};

constexpr SubclassDescriptor kClassSerialBusSubclasses[] = {
    {0x00, "FireWire (IEEE 1394) controller", nullptr, 0},
    {0x01, "ACCESS bus controller", nullptr, 0},
    {0x02, "SSA", nullptr, 0},
    {0x03, "USB controller", kSerialBusUsbProgIfs,
     sizeof(kSerialBusUsbProgIfs) / sizeof(kSerialBusUsbProgIfs[0])},
    {0x04, "Fibre Channel", nullptr, 0},
    {0x05, "SMBus controller", nullptr, 0},
    {0x06, "InfiniBand controller", nullptr, 0},
    {0x07, "IPMI interface", nullptr, 0},
    {0x08, "SERCOS interface", nullptr, 0},
    {0x09, "CANbus controller", nullptr, 0},
    {0x80, "Other serial bus controller", nullptr, 0},
};

constexpr SubclassDescriptor kClassWirelessSubclasses[] = {
    {0x00, "IRDA controller", nullptr, 0},
    {0x01, "Consumer IR controller", nullptr, 0},
    {0x10, "RF controller", nullptr, 0},
    {0x11, "Bluetooth controller", nullptr, 0},
    {0x12, "Broadband controller", nullptr, 0},
    {0x20, "Ethernet controller (802.11a)", nullptr, 0},
    {0x21, "Ethernet controller (802.11b)", nullptr, 0},
    {0x80, "Other wireless controller", nullptr, 0},
};

constexpr ClassDescriptor kClassTable[] = {
    {0x00, "Unclassified device", kClassUnclassifiedSubclasses,
     sizeof(kClassUnclassifiedSubclasses) / sizeof(kClassUnclassifiedSubclasses[0])},
    {0x01, "Mass storage controller", kClassMassStorageSubclasses,
     sizeof(kClassMassStorageSubclasses) / sizeof(kClassMassStorageSubclasses[0])},
    {0x02, "Network controller", kClassNetworkSubclasses,
     sizeof(kClassNetworkSubclasses) / sizeof(kClassNetworkSubclasses[0])},
    {0x03, "Display controller", kClassDisplaySubclasses,
     sizeof(kClassDisplaySubclasses) / sizeof(kClassDisplaySubclasses[0])},
    {0x04, "Multimedia controller", kClassMultimediaSubclasses,
     sizeof(kClassMultimediaSubclasses) / sizeof(kClassMultimediaSubclasses[0])},
    {0x05, "Memory controller", kClassMemorySubclasses,
     sizeof(kClassMemorySubclasses) / sizeof(kClassMemorySubclasses[0])},
    {0x06, "Bridge device", kClassBridgeSubclasses,
     sizeof(kClassBridgeSubclasses) / sizeof(kClassBridgeSubclasses[0])},
    {0x07, "Simple communication controller", kClassSimpleCommSubclasses,
     sizeof(kClassSimpleCommSubclasses) / sizeof(kClassSimpleCommSubclasses[0])},
    {0x08, "Base system peripheral", kClassBaseSystemSubclasses,
     sizeof(kClassBaseSystemSubclasses) / sizeof(kClassBaseSystemSubclasses[0])},
    {0x09, "Input device controller", kClassInputDeviceSubclasses,
     sizeof(kClassInputDeviceSubclasses) / sizeof(kClassInputDeviceSubclasses[0])},
    {0x0C, "Serial bus controller", kClassSerialBusSubclasses,
     sizeof(kClassSerialBusSubclasses) / sizeof(kClassSerialBusSubclasses[0])},
    {0x0D, "Wireless controller", kClassWirelessSubclasses,
     sizeof(kClassWirelessSubclasses) / sizeof(kClassWirelessSubclasses[0])},
};

constexpr size_t kClassTableCount =
    sizeof(kClassTable) / sizeof(kClassTable[0]);

const ClassDescriptor* find_class_descriptor(uint8_t class_code) {
    for (size_t i = 0; i < kClassTableCount; ++i) {
        if (kClassTable[i].value == class_code) {
            return &kClassTable[i];
        }
    }
    return nullptr;
}

const SubclassDescriptor* find_subclass_descriptor(const ClassDescriptor& cls,
                                                   uint8_t subclass) {
    for (size_t i = 0; i < cls.subclass_count; ++i) {
        if (cls.subclasses[i].value == subclass) {
            return &cls.subclasses[i];
        }
    }
    return nullptr;
}

const ProgIfDescriptor* find_prog_if_descriptor(const SubclassDescriptor& sub,
                                                uint8_t prog_if) {
    for (size_t i = 0; i < sub.prog_if_count; ++i) {
        if (sub.prog_ifs[i].value == prog_if) {
            return &sub.prog_ifs[i];
        }
    }
    return nullptr;
}

uint32_t build_config_address(uint8_t bus,
                              uint8_t slot,
                              uint8_t function,
                              uint8_t offset) {
    uint32_t aligned_offset = static_cast<uint32_t>(offset & 0xFC);
    return 0x80000000u |
           (static_cast<uint32_t>(bus) << 16) |
           (static_cast<uint32_t>(slot) << 11) |
           (static_cast<uint32_t>(function) << 8) |
           aligned_offset;
}

void register_device(const pci::PciDevice& info) {
    if (g_device_count >= kMaxDeviceCount) {
        log_message(LogLevel::Warn,
                    "PCI: device table full (capacity %zu)",
                    static_cast<size_t>(kMaxDeviceCount));
        return;
    }
    g_devices[g_device_count++] = info;
}

uint32_t read_config32_raw(uint8_t bus,
                           uint8_t slot,
                           uint8_t function,
                           uint8_t offset) {
    uint32_t config_address = build_config_address(bus, slot, function, offset);
    outl(kConfigAddressPort, config_address);
    return inl(kConfigDataPort);
}

uint16_t read_config16_raw(uint8_t bus,
                           uint8_t slot,
                           uint8_t function,
                           uint8_t offset) {
    uint32_t value = read_config32_raw(bus, slot, function, offset);
    uint32_t shift = static_cast<uint32_t>(offset & 0x02u) * 8;
    return static_cast<uint16_t>((value >> shift) & 0xFFFFu);
}

uint8_t read_config8_raw(uint8_t bus,
                         uint8_t slot,
                         uint8_t function,
                         uint8_t offset) {
    uint32_t value = read_config32_raw(bus, slot, function, offset);
    uint32_t shift = static_cast<uint32_t>(offset & 0x03u) * 8;
    return static_cast<uint8_t>((value >> shift) & 0xFFu);
}

void enumerate_function(uint8_t bus, uint8_t slot, uint8_t function) {
    uint16_t vendor_id = read_config16_raw(bus, slot, function, 0x00);
    if (vendor_id == 0xFFFF) {
        return;
    }

    uint16_t device_id = read_config16_raw(bus, slot, function, 0x02);
    uint32_t class_reg = read_config32_raw(bus, slot, function, 0x08);

    pci::PciDevice info{};
    info.bus = bus;
    info.slot = slot;
    info.function = function;
    info.vendor = vendor_id;
    info.device = device_id;
    info.class_code = static_cast<uint8_t>((class_reg >> 24) & 0xFF);
    info.subclass = static_cast<uint8_t>((class_reg >> 16) & 0xFF);
    info.prog_if = static_cast<uint8_t>((class_reg >> 8) & 0xFF);
    info.revision = static_cast<uint8_t>(class_reg & 0xFF);

    register_device(info);

    const char* class_str = pci::class_name(info.class_code);
    const char* subclass_str =
        pci::subclass_name(info.class_code, info.subclass);
    const char* prog_if_str =
        pci::prog_if_name(info.class_code, info.subclass, info.prog_if);

    log_message(LogLevel::Info,
                "PCI: %02u:%02u.%u vendor=%04x device=%04x class=%u.%u.%u (%s / %s / %s) rev=%02x",
                static_cast<unsigned int>(info.bus),
                static_cast<unsigned int>(info.slot),
                static_cast<unsigned int>(info.function),
                static_cast<unsigned int>(info.vendor),
                static_cast<unsigned int>(info.device),
                static_cast<unsigned int>(info.class_code),
                static_cast<unsigned int>(info.subclass),
                static_cast<unsigned int>(info.prog_if),
                class_str,
                subclass_str,
                prog_if_str,
                static_cast<unsigned int>(info.revision));
}

void enumerate_bus(uint8_t bus) {
    for (uint8_t slot = 0; slot < 32; ++slot) {
        uint16_t vendor_id = read_config16_raw(bus, slot, 0, 0x00);
        if (vendor_id == 0xFFFF) {
            continue;
        }

        enumerate_function(bus, slot, 0);

        uint8_t header_type = read_config8_raw(bus, slot, 0, 0x0E);
        if ((header_type & 0x80u) == 0) {
            continue;
        }

        for (uint8_t function = 1; function < 8; ++function) {
            if (read_config16_raw(bus, slot, function, 0x00) == 0xFFFF) {
                continue;
            }
            enumerate_function(bus, slot, function);
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

const PciDevice* devices() {
    return g_devices;
}

const PciDevice* find_device(uint16_t vendor_id, uint16_t device_id) {
    for (size_t i = 0; i < g_device_count; ++i) {
        if (g_devices[i].vendor == vendor_id &&
            g_devices[i].device == device_id) {
            return &g_devices[i];
        }
    }
    return nullptr;
}

const PciDevice* find_by_class(uint8_t class_code,
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

uint32_t read_config32(uint8_t bus, uint8_t slot, uint8_t function,
                       uint8_t offset) {
    return read_config32_raw(bus, slot, function, offset);
}

uint16_t read_config16(uint8_t bus, uint8_t slot, uint8_t function,
                       uint8_t offset) {
    return read_config16_raw(bus, slot, function, offset);
}

uint8_t read_config8(uint8_t bus, uint8_t slot, uint8_t function,
                     uint8_t offset) {
    return read_config8_raw(bus, slot, function, offset);
}

uint32_t read_config32(const PciDevice& device, uint8_t offset) {
    return read_config32(device.bus, device.slot, device.function, offset);
}

uint16_t read_config16(const PciDevice& device, uint8_t offset) {
    return read_config16(device.bus, device.slot, device.function, offset);
}

uint8_t read_config8(const PciDevice& device, uint8_t offset) {
    return read_config8(device.bus, device.slot, device.function, offset);
}

const char* class_name(uint8_t class_code) {
    const auto* cls = find_class_descriptor(class_code);
    return (cls != nullptr) ? cls->name : "Unknown class";
}

const char* subclass_name(uint8_t class_code, uint8_t subclass) {
    const auto* cls = find_class_descriptor(class_code);
    if (cls == nullptr) {
        return "Unknown subclass";
    }
    const auto* sub = find_subclass_descriptor(*cls, subclass);
    return (sub != nullptr) ? sub->name : "Unknown subclass";
}

const char* prog_if_name(uint8_t class_code, uint8_t subclass,
                         uint8_t prog_if) {
    const auto* cls = find_class_descriptor(class_code);
    if (cls == nullptr) {
        return "Unknown programming interface";
    }
    const auto* sub = find_subclass_descriptor(*cls, subclass);
    if (sub == nullptr) {
        return "Unknown programming interface";
    }
    if (sub->prog_if_count == 0 || sub->prog_ifs == nullptr) {
        return "N/A";
    }
    const auto* prog = find_prog_if_descriptor(*sub, prog_if);
    return (prog != nullptr) ? prog->name : "Unknown programming interface";
}

}  // namespace pci
