#pragma once

namespace acpi {

// Makes firmware tables available before the kernel heap exists.
bool initialize_tables();
// Starts the AML namespace and runtime services after core kernel init.
bool initialize();

}  // namespace acpi
