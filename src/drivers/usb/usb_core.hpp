#pragma once

#include <stddef.h>
#include <stdint.h>

namespace usb {

enum class Speed : uint8_t {
    Unknown,
    Low,
    Full,
    High,
    Super,
};

enum class EndpointType : uint8_t {
    Control,
    Isochronous,
    Bulk,
    Interrupt,
};

enum class TransferStatus : uint8_t {
    Ok,
    Stall,
    Timeout,
    NoDevice,
    IoError,
    Unsupported,
};

struct ControlRequest {
    uint8_t request_type;
    uint8_t request;
    uint16_t value;
    uint16_t index;
    uint16_t length;
};

using ControlTransferFn =
    TransferStatus (*)(void* context, const ControlRequest& request, void* data);
using BulkTransferFn =
    TransferStatus (*)(void* context, uint8_t endpoint, void* data,
                       size_t length, size_t& transferred);

struct Transport {
    void* context;
    ControlTransferFn control;
    BulkTransferFn bulk;
};

struct Endpoint {
    uint8_t address;
    EndpointType type;
    uint16_t max_packet_size;
    uint8_t interval;
};

struct Interface {
    uint8_t number;
    uint8_t alternate_setting;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t protocol;
};

struct Device {
    uint8_t address;
    Speed speed;
    uint16_t vendor_id;
    uint16_t product_id;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t protocol;
    Interface interfaces[8];
    size_t interface_count;
    Endpoint endpoints[16];
    size_t endpoint_count;
    Transport transport;
};

using ClassProbeFn = bool (*)(const Device& device);

bool register_class_driver(const char* name, ClassProbeFn probe);
bool register_device(const Device& device);

size_t device_count();
const Device* device_at(size_t index);

const char* speed_name(Speed speed);

}  // namespace usb
