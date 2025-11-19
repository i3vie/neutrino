#pragma once
#include <stddef.h>
#include <stdint.h>

namespace pci {

struct PciDevice {
    uint8_t bus;
    uint8_t slot;
    uint8_t function;
    uint16_t vendor;
    uint16_t device;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint8_t revision;
};

void init();

size_t device_count();

const PciDevice* devices();

const PciDevice* find_device(uint16_t vendor_id, uint16_t device_id);

const PciDevice* find_by_class(uint8_t class_code,
                               uint8_t subclass,
                               uint8_t prog_if = 0xFF);

uint32_t read_config32(uint8_t bus, uint8_t slot, uint8_t function,
                       uint8_t offset);
uint16_t read_config16(uint8_t bus, uint8_t slot, uint8_t function,
                       uint8_t offset);
uint8_t read_config8(uint8_t bus, uint8_t slot, uint8_t function,
                     uint8_t offset);

void write_config32(uint8_t bus, uint8_t slot, uint8_t function,
                    uint8_t offset, uint32_t value);
void write_config16(uint8_t bus, uint8_t slot, uint8_t function,
                    uint8_t offset, uint16_t value);
void write_config8(uint8_t bus, uint8_t slot, uint8_t function,
                   uint8_t offset, uint8_t value);

uint32_t read_config32(const PciDevice& device, uint8_t offset);
uint16_t read_config16(const PciDevice& device, uint8_t offset);
uint8_t read_config8(const PciDevice& device, uint8_t offset);

void write_config32(const PciDevice& device, uint8_t offset, uint32_t value);
void write_config16(const PciDevice& device, uint8_t offset, uint16_t value);
void write_config8(const PciDevice& device, uint8_t offset, uint8_t value);

const char* class_name(uint8_t class_code);
const char* subclass_name(uint8_t class_code, uint8_t subclass);
const char* prog_if_name(uint8_t class_code, uint8_t subclass,
                         uint8_t prog_if);

}  // namespace pci
