#pragma once

#include <stddef.h>
#include <stdint.h>

#include "kernel/module.hpp"

namespace driver_registry {

using InitFn = void (*)();
using PciMatch = kernel_module::PciMatch;

constexpr uint16_t kAnyVendor = kernel_module::kAnyVendor;
constexpr uint16_t kAnyDevice = kernel_module::kAnyDevice;
constexpr uint8_t kAnyClass = kernel_module::kAnyClass;
constexpr uint8_t kAnySubclass = kernel_module::kAnySubclass;
constexpr uint8_t kAnyProgIf = kernel_module::kAnyProgIf;

bool register_pci_driver(const char* name,
                         const PciMatch* matches,
                         size_t match_count,
                         InitFn init);

void probe_pci_drivers();
void start_pci_probe_worker();

}  // namespace driver_registry
