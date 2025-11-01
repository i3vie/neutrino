#pragma once
#include <stddef.h>
#include <stdint.h>

namespace pci {

struct DeviceAddress {
    uint8_t bus;
    uint8_t device;
    uint8_t function;
};

struct DeviceInfo {
    DeviceAddress address;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint8_t revision;
};

void init();

size_t device_count();

const DeviceInfo* devices();

const DeviceInfo* find_device(uint16_t vendor_id, uint16_t device_id);

const DeviceInfo* find_by_class(uint8_t class_code,
                                uint8_t subclass,
                                uint8_t prog_if = 0xFF);

uint32_t read_config32(const DeviceAddress& address, uint8_t offset);
uint16_t read_config16(const DeviceAddress& address, uint8_t offset);
uint8_t read_config8(const DeviceAddress& address, uint8_t offset);

}  // namespace pci

