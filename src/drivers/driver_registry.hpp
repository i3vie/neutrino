#pragma once

#include <stddef.h>
#include <stdint.h>

namespace driver_registry {

using InitFn = void (*)();

struct PciMatch {
    uint16_t vendor;
    uint16_t device;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
};

constexpr uint16_t kAnyVendor = 0xFFFFu;
constexpr uint16_t kAnyDevice = 0xFFFFu;
constexpr uint8_t kAnyClass = 0xFFu;
constexpr uint8_t kAnySubclass = 0xFFu;
constexpr uint8_t kAnyProgIf = 0xFFu;

bool register_pci_driver(const char* name,
                         const PciMatch* matches,
                         size_t match_count,
                         InitFn init);

void probe_pci_drivers();
void start_pci_probe_worker();

}  // namespace driver_registry
