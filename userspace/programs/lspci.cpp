#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "descriptors.hpp"
#include "../crt/syscall.hpp"

namespace {

constexpr uint32_t kDescConsole =
    static_cast<uint32_t>(descriptor_defs::Type::Console);
constexpr uint32_t kDescPci =
    static_cast<uint32_t>(descriptor_defs::Type::Pci);

void print(long console, const char* text) {
    if (console >= 0 && text != nullptr) {
        descriptor_write(static_cast<uint32_t>(console), text, strlen(text));
    }
}

void print_hex(long console, uint32_t value, size_t digits) {
    constexpr char hex[] = "0123456789abcdef";
    char buffer[9];
    if (digits > 8) {
        digits = 8;
    }
    for (size_t i = 0; i < digits; ++i) {
        size_t shift = (digits - i - 1) * 4;
        buffer[i] = hex[(value >> shift) & 0x0f];
    }
    buffer[digits] = '\0';
    print(console, buffer);
}

const char* class_name(uint8_t code) {
    switch (code) {
        case 0x00: return "Unclassified device";
        case 0x01: return "Mass storage controller";
        case 0x02: return "Network controller";
        case 0x03: return "Display controller";
        case 0x04: return "Multimedia controller";
        case 0x05: return "Memory controller";
        case 0x06: return "Bridge device";
        case 0x07: return "Communication controller";
        case 0x08: return "System peripheral";
        case 0x09: return "Input device controller";
        case 0x0a: return "Docking station";
        case 0x0b: return "Processor";
        case 0x0c: return "Serial bus controller";
        case 0x0d: return "Wireless controller";
        case 0x0e: return "Intelligent controller";
        case 0x0f: return "Satellite controller";
        case 0x10: return "Encryption controller";
        case 0x11: return "Signal processing controller";
        case 0x12: return "Processing accelerator";
        case 0x13: return "Instrumentation controller";
        default: return "Unknown device";
    }
}

const char* subclass_name(uint8_t cls, uint8_t sub) {
    if (cls == 0x01) {
        switch (sub) {
            case 0x00: return "SCSI storage controller";
            case 0x01: return "IDE interface";
            case 0x04: return "RAID bus controller";
            case 0x05: return "ATA controller";
            case 0x06: return "SATA controller";
            case 0x07: return "SAS controller";
            case 0x08: return "Non-Volatile memory controller";
        }
    } else if (cls == 0x02 && sub == 0x00) {
        return "Ethernet controller";
    } else if (cls == 0x03) {
        switch (sub) {
            case 0x00: return "VGA compatible controller";
            case 0x01: return "XGA compatible controller";
            case 0x02: return "3D controller";
        }
    } else if (cls == 0x04) {
        switch (sub) {
            case 0x00: return "Multimedia video controller";
            case 0x01: return "Multimedia audio controller";
            case 0x03: return "Audio device";
        }
    } else if (cls == 0x06) {
        switch (sub) {
            case 0x00: return "Host bridge";
            case 0x01: return "ISA bridge";
            case 0x04: return "PCI bridge";
            case 0x07: return "CardBus bridge";
        }
    } else if (cls == 0x08) {
        switch (sub) {
            case 0x00: return "PIC";
            case 0x01: return "DMA controller";
            case 0x02: return "Timer";
            case 0x03: return "RTC";
            case 0x05: return "SD host controller";
            case 0x06: return "IOMMU";
        }
    } else if (cls == 0x0c) {
        switch (sub) {
            case 0x03: return "USB controller";
            case 0x05: return "SMBus";
        }
    }
    return class_name(cls);
}

const char* prog_if_name(uint8_t cls, uint8_t sub, uint8_t prog_if) {
    if (cls == 0x01 && sub == 0x06) {
        if (prog_if == 0x01) return "AHCI 1.0";
        if (prog_if == 0x02) return "Serial Storage Bus";
    }
    if (cls == 0x0c && sub == 0x03) {
        switch (prog_if) {
            case 0x00: return "UHCI";
            case 0x10: return "OHCI";
            case 0x20: return "EHCI";
            case 0x30: return "xHCI";
        }
    }
    return nullptr;
}

struct Options {
    bool numeric;
    bool verbose;
    bool valid;
};

Options parse_options(const char* args) {
    Options options{false, false, true};
    const char* cursor = args == nullptr ? "" : args;
    while (*cursor != '\0') {
        while (*cursor == ' ' || *cursor == '\t' ||
               *cursor == '\r' || *cursor == '\n') {
            ++cursor;
        }
        if (*cursor == '\0') break;
        if (*cursor++ != '-') {
            options.valid = false;
            break;
        }
        if (*cursor == '-') {
            options.valid = false;
            break;
        }
        while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t' &&
               *cursor != '\r' && *cursor != '\n') {
            if (*cursor == 'n') options.numeric = true;
            else if (*cursor == 'v') options.verbose = true;
            else options.valid = false;
            ++cursor;
        }
    }
    return options;
}

void print_device(long console,
                  const descriptor_defs::PciDeviceInfo& device,
                  const Options& options) {
    print_hex(console, device.bus, 2);
    print(console, ":");
    print_hex(console, device.slot, 2);
    print(console, ".");
    print_hex(console, device.function, 1);
    print(console, " ");

    if (!options.numeric) {
        print(console, subclass_name(device.class_code, device.subclass));
        print(console, " [");
    }
    print_hex(console, device.class_code, 2);
    print_hex(console, device.subclass, 2);
    if (!options.numeric) print(console, "]");
    print(console, ": ");
    print_hex(console, device.vendor_id, 4);
    print(console, ":");
    print_hex(console, device.device_id, 4);
    print(console, " (rev ");
    print_hex(console, device.revision, 2);
    print(console, ")\n");

    if (options.verbose) {
        print(console, "\tClass ");
        print(console, class_name(device.class_code));
        print(console, ", programming interface ");
        print_hex(console, device.prog_if, 2);
        const char* name = prog_if_name(device.class_code,
                                        device.subclass,
                                        device.prog_if);
        if (name != nullptr) {
            print(console, " (");
            print(console, name);
            print(console, ")");
        }
        print(console, "\n");
    }
}

}  // namespace

int main(uint64_t arg_ptr, uint64_t) {
    long console = process_get_standard_descriptor(1);
    if (console < 0) {
        console = descriptor_open(kDescConsole, 0);
    }

    Options options = parse_options(reinterpret_cast<const char*>(arg_ptr));
    if (!options.valid) {
        print(console, "usage: lspci [-n] [-v]\n");
        return 1;
    }

    long pci = descriptor_open(kDescPci, 0);
    if (pci < 0) {
        print(console, "lspci: PCI inventory is unavailable\n");
        return 1;
    }

    descriptor_defs::PciDeviceInfo devices[32];
    uint64_t offset = 0;
    bool any = false;
    for (;;) {
        long bytes = descriptor_read(static_cast<uint32_t>(pci),
                                     devices,
                                     sizeof(devices),
                                     offset);
        if (bytes < 0 ||
            (bytes % static_cast<long>(sizeof(devices[0]))) != 0) {
            descriptor_close(static_cast<uint32_t>(pci));
            print(console, "lspci: failed to read PCI inventory\n");
            return 1;
        }
        if (bytes == 0) break;
        size_t count = static_cast<size_t>(bytes) / sizeof(devices[0]);
        for (size_t i = 0; i < count; ++i) {
            print_device(console, devices[i], options);
            any = true;
        }
        offset += static_cast<uint64_t>(bytes);
    }

    descriptor_close(static_cast<uint32_t>(pci));
    if (!any) {
        print(console, "lspci: no PCI devices found\n");
        return 1;
    }
    return 0;
}
