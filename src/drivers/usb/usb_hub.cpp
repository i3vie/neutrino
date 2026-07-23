#include "usb_hub.hpp"

#include <stddef.h>
#include <stdint.h>

#include "drivers/log/logging.hpp"
#include "kernel/module.hpp"
#include "kernel/time.hpp"

namespace usb::hub {
namespace {

constexpr uint8_t kClassHub = 0x09;
constexpr uint8_t kDescriptorHub = 0x29;
constexpr uint8_t kRequestGetStatus = 0x00;
constexpr uint8_t kRequestClearFeature = 0x01;
constexpr uint8_t kRequestSetFeature = 0x03;
constexpr uint8_t kRequestGetDescriptor = 0x06;
constexpr uint8_t kRequestTypeHubIn = 0xA0;
constexpr uint8_t kRequestTypePortIn = 0xA3;
constexpr uint8_t kRequestTypePortOut = 0x23;
constexpr uint16_t kPortFeatureReset = 4;
constexpr uint16_t kPortFeaturePower = 8;
constexpr uint16_t kPortFeatureConnectionChange = 16;
constexpr uint16_t kPortFeatureEnableChange = 17;
constexpr uint16_t kPortFeatureSuspendChange = 18;
constexpr uint16_t kPortFeatureOverCurrentChange = 19;
constexpr uint16_t kPortFeatureResetChange = 20;
constexpr uint16_t kPortStatusConnection = 1u << 0;
constexpr uint16_t kPortStatusEnable = 1u << 1;
constexpr uint16_t kPortStatusLowSpeed = 1u << 9;
constexpr uint16_t kPortStatusHighSpeed = 1u << 10;
constexpr uint16_t kPortChangeReset = 1u << 4;
constexpr uint8_t kMaxSupportedPorts = 15;
constexpr uint32_t kResetTimeoutMs = 200;
constexpr uint64_t kNanosecondsPerMillisecond = 1000000ull;

struct PortStatus {
    uint16_t status;
    uint16_t change;
};

bool control(const usb::Device& device,
             uint8_t request_type,
             uint8_t request,
             uint16_t value,
             uint16_t index,
             void* data,
             uint16_t length) {
    if (device.transport.control == nullptr) {
        return false;
    }
    usb::ControlRequest control_request{
        request_type,
        request,
        value,
        index,
        length,
    };
    return device.transport.control(device.transport.context,
                                    control_request,
                                    data) == usb::TransferStatus::Ok;
}

void delay_milliseconds(uint32_t milliseconds) {
    uint64_t start = timekeeping::nanoseconds_since_boot();
    uint64_t duration = static_cast<uint64_t>(milliseconds) *
                        kNanosecondsPerMillisecond;
    while (timekeeping::nanoseconds_since_boot() - start < duration) {
        asm volatile("pause");
    }
}

bool set_port_feature(const usb::Device& device,
                      uint8_t port,
                      uint16_t feature) {
    return control(device,
                   kRequestTypePortOut,
                   kRequestSetFeature,
                   feature,
                   port,
                   nullptr,
                   0);
}

bool clear_port_feature(const usb::Device& device,
                        uint8_t port,
                        uint16_t feature) {
    return control(device,
                   kRequestTypePortOut,
                   kRequestClearFeature,
                   feature,
                   port,
                   nullptr,
                   0);
}

bool get_port_status(const usb::Device& device,
                     uint8_t port,
                     PortStatus& status) {
    uint8_t bytes[4]{};
    if (!control(device,
                 kRequestTypePortIn,
                 kRequestGetStatus,
                 0,
                 port,
                 bytes,
                 sizeof(bytes))) {
        return false;
    }
    status.status = static_cast<uint16_t>(bytes[0]) |
                    (static_cast<uint16_t>(bytes[1]) << 8);
    status.change = static_cast<uint16_t>(bytes[2]) |
                    (static_cast<uint16_t>(bytes[3]) << 8);
    return true;
}

usb::Speed port_speed(const PortStatus& status) {
    if ((status.status & kPortStatusLowSpeed) != 0) {
        return usb::Speed::Low;
    }
    if ((status.status & kPortStatusHighSpeed) != 0) {
        return usb::Speed::High;
    }
    return usb::Speed::Full;
}

void clear_port_changes(const usb::Device& device,
                        uint8_t port,
                        uint16_t changes) {
    constexpr uint16_t features[] = {
        kPortFeatureConnectionChange,
        kPortFeatureEnableChange,
        kPortFeatureSuspendChange,
        kPortFeatureOverCurrentChange,
        kPortFeatureResetChange,
    };
    for (size_t i = 0; i < sizeof(features) / sizeof(features[0]); ++i) {
        uint16_t change_bit = static_cast<uint16_t>(1u << i);
        if ((changes & change_bit) != 0) {
            (void)clear_port_feature(device, port, features[i]);
        }
    }
}

bool reset_port(const usb::Device& device,
                uint8_t port,
                PortStatus& status) {
    if (!set_port_feature(device, port, kPortFeatureReset)) {
        return false;
    }

    uint64_t start = timekeeping::nanoseconds_since_boot();
    uint64_t timeout = static_cast<uint64_t>(kResetTimeoutMs) *
                       kNanosecondsPerMillisecond;
    for (uint32_t attempts = 0; attempts < kResetTimeoutMs; ++attempts) {
        delay_milliseconds(1);
        if (!get_port_status(device, port, status)) {
            return false;
        }
        if ((status.change & kPortChangeReset) != 0 &&
            (status.status & kPortStatusEnable) != 0) {
            (void)clear_port_feature(device,
                                     port,
                                     kPortFeatureResetChange);
            return true;
        }
        if (timekeeping::nanoseconds_since_boot() - start >= timeout) {
            break;
        }
    }
    return false;
}

bool find_hub_interface(const usb::Device& device, uint8_t& protocol) {
    if (device.class_code == kClassHub) {
        protocol = device.protocol;
        return true;
    }
    for (size_t i = 0; i < device.interface_count; ++i) {
        if (device.interfaces[i].class_code == kClassHub) {
            protocol = device.interfaces[i].protocol;
            return true;
        }
    }
    return false;
}

}  // namespace

void init() {
    (void)usb::register_class_driver("usb-hub", probe_device);
}

bool init_module() {
    init();
    return true;
}

KERNEL_BUILTIN_MODULE(usb_hub_module,
                      "usb-hub",
                      kernel_module::Phase::Bus,
                      init_module,
                      nullptr,
                      0);

bool probe_device(const usb::Device& device) {
    uint8_t protocol = 0;
    if (!find_hub_interface(device, protocol)) {
        return false;
    }
    if (device.speed == usb::Speed::Super || protocol == 3) {
        log_message(LogLevel::Warn,
                    "usb-hub: SuperSpeed hub addr=%u is not supported yet",
                    static_cast<unsigned int>(device.address));
        return false;
    }
    if (device.transport.configure_hub == nullptr ||
        device.transport.enumerate_hub_port == nullptr) {
        log_message(LogLevel::Warn,
                    "usb-hub: host controller lacks hub support addr=%u",
                    static_cast<unsigned int>(device.address));
        return false;
    }

    uint8_t descriptor[7]{};
    if (!control(device,
                 kRequestTypeHubIn,
                 kRequestGetDescriptor,
                 static_cast<uint16_t>(kDescriptorHub) << 8,
                 0,
                 descriptor,
                 sizeof(descriptor)) ||
        descriptor[0] < sizeof(descriptor) ||
        descriptor[1] != kDescriptorHub) {
        log_message(LogLevel::Warn,
                    "usb-hub: failed to read descriptor addr=%u",
                    static_cast<unsigned int>(device.address));
        return false;
    }

    uint8_t port_count = descriptor[2];
    if (port_count == 0 || port_count > kMaxSupportedPorts) {
        log_message(LogLevel::Warn,
                    "usb-hub: unsupported port count %u addr=%u",
                    static_cast<unsigned int>(port_count),
                    static_cast<unsigned int>(device.address));
        return false;
    }
    uint16_t characteristics = static_cast<uint16_t>(descriptor[3]) |
                               (static_cast<uint16_t>(descriptor[4]) << 8);
    uint8_t power_switching = static_cast<uint8_t>(characteristics & 0x3u);
    uint8_t tt_think_time = static_cast<uint8_t>((characteristics >> 5) & 0x3u);
    bool multi_tt = protocol == 2;
    usb::TransferStatus configure_status = device.transport.configure_hub(
        device.transport.context, port_count, multi_tt, tt_think_time);
    if (configure_status != usb::TransferStatus::Ok) {
        log_message(LogLevel::Warn,
                    "usb-hub: failed to configure xHCI hub context addr=%u status=%u",
                    static_cast<unsigned int>(device.address),
                    static_cast<unsigned int>(configure_status));
        return false;
    }

    if (power_switching != 2) {
        for (uint8_t port = 1; port <= port_count; ++port) {
            if (!set_port_feature(device, port, kPortFeaturePower)) {
                log_message(LogLevel::Warn,
                            "usb-hub: failed to power addr=%u port=%u",
                            static_cast<unsigned int>(device.address),
                            static_cast<unsigned int>(port));
                return false;
            }
        }
    }
    delay_milliseconds(static_cast<uint32_t>(descriptor[5]) * 2u);

    log_message(LogLevel::Info,
                "usb-hub: addr=%u ports=%u tt=%s",
                static_cast<unsigned int>(device.address),
                static_cast<unsigned int>(port_count),
                multi_tt ? "multi" : "single");
    for (uint8_t port = 1; port <= port_count; ++port) {
        PortStatus status{};
        if (!get_port_status(device, port, status)) {
            log_message(LogLevel::Warn,
                        "usb-hub: status read failed addr=%u port=%u",
                        static_cast<unsigned int>(device.address),
                        static_cast<unsigned int>(port));
            continue;
        }
        clear_port_changes(device, port, status.change);
        if ((status.status & kPortStatusConnection) == 0) {
            continue;
        }
        if (!reset_port(device, port, status)) {
            log_message(LogLevel::Warn,
                        "usb-hub: reset failed addr=%u port=%u status=%04x change=%04x",
                        static_cast<unsigned int>(device.address),
                        static_cast<unsigned int>(port),
                        static_cast<unsigned int>(status.status),
                        static_cast<unsigned int>(status.change));
            continue;
        }

        usb::Speed speed = port_speed(status);
        usb::TransferStatus enumeration_status =
            device.transport.enumerate_hub_port(device.transport.context,
                                                port,
                                                speed);
        if (enumeration_status != usb::TransferStatus::Ok) {
            log_message(LogLevel::Warn,
                        "usb-hub: enumeration failed addr=%u port=%u status=%u",
                        static_cast<unsigned int>(device.address),
                        static_cast<unsigned int>(port),
                        static_cast<unsigned int>(enumeration_status));
        }
    }
    return true;
}

}  // namespace usb::hub
