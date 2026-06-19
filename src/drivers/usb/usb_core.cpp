#include "usb_core.hpp"

#include "drivers/log/logging.hpp"

namespace usb {
namespace {

struct ClassDriver {
    const char* name;
    ClassProbeFn probe;
    bool used;
};

constexpr size_t kMaxDevices = 32;
constexpr size_t kMaxClassDrivers = 16;

Device g_devices[kMaxDevices]{};
size_t g_device_count = 0;
ClassDriver g_class_drivers[kMaxClassDrivers]{};

}  // namespace

bool register_class_driver(const char* name, ClassProbeFn probe) {
    if (name == nullptr || probe == nullptr) {
        return false;
    }
    for (size_t i = 0; i < kMaxClassDrivers; ++i) {
        if (g_class_drivers[i].used) {
            continue;
        }
        g_class_drivers[i] = ClassDriver{
            .name = name,
            .probe = probe,
            .used = true,
        };
        return true;
    }
    log_message(LogLevel::Warn, "usb: class driver table full for %s", name);
    return false;
}

bool register_device(const Device& device) {
    if (g_device_count >= kMaxDevices) {
        log_message(LogLevel::Warn, "usb: device table full");
        return false;
    }

    Device& stored = g_devices[g_device_count++];
    stored = device;

    log_message(LogLevel::Info,
                "usb: device addr=%u speed=%s vid=%04x pid=%04x interfaces=%zu endpoints=%zu",
                static_cast<unsigned int>(stored.address),
                speed_name(stored.speed),
                static_cast<unsigned int>(stored.vendor_id),
                static_cast<unsigned int>(stored.product_id),
                stored.interface_count,
                stored.endpoint_count);

    for (size_t i = 0; i < kMaxClassDrivers; ++i) {
        if (!g_class_drivers[i].used || g_class_drivers[i].probe == nullptr) {
            continue;
        }
        if (g_class_drivers[i].probe(stored)) {
            log_message(LogLevel::Info,
                        "usb: %s claimed device addr=%u",
                        g_class_drivers[i].name,
                        static_cast<unsigned int>(stored.address));
            return true;
        }
    }
    log_message(LogLevel::Info,
                "usb: no class driver claimed device addr=%u",
                static_cast<unsigned int>(stored.address));
    return true;
}

size_t device_count() {
    return g_device_count;
}

const Device* device_at(size_t index) {
    if (index >= g_device_count) {
        return nullptr;
    }
    return &g_devices[index];
}

const char* speed_name(Speed speed) {
    switch (speed) {
        case Speed::Low:
            return "low";
        case Speed::Full:
            return "full";
        case Speed::High:
            return "high";
        case Speed::Super:
            return "super";
        default:
            return "unknown";
    }
}

}  // namespace usb
