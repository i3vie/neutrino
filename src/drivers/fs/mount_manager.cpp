#include "mount_manager.hpp"

#include <stddef.h>

#include "drivers/log/logging.hpp"
#include "drivers/storage/ide_provider.hpp"
#include "drivers/fs/fat32/driver.hpp"

namespace fs {
namespace {

constexpr size_t kMaxProviders = 8;
constexpr size_t kMaxFilesystemDrivers = 8;
constexpr size_t kMaxDiscoveredDevices = 32;

BlockDeviceEnumerateFn g_providers[kMaxProviders];
size_t g_provider_count = 0;

FilesystemProbeFn g_filesystem_drivers[kMaxFilesystemDrivers];
size_t g_filesystem_driver_count = 0;

bool g_builtins_registered = false;

void ensure_builtins_registered() {
    if (g_builtins_registered) {
        return;
    }
    register_ide_block_device_provider();
    register_fat32_filesystem_driver();
    g_builtins_registered = true;
}

bool strings_equal(const char* a, const char* b) {
    if (a == nullptr || b == nullptr) {
        return false;
    }
    while (*a && *b) {
        if (*a != *b) {
            return false;
        }
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

}  // namespace

void register_block_device_provider(BlockDeviceEnumerateFn fn) {
    if (fn == nullptr) {
        log_message(LogLevel::Warn,
                    "MountManager: attempted to register null block provider");
        return;
    }
    if (g_provider_count >= kMaxProviders) {
        log_message(LogLevel::Warn,
                    "MountManager: block provider registry is full");
        return;
    }
    g_providers[g_provider_count++] = fn;
}

void register_filesystem_driver(FilesystemProbeFn fn) {
    if (fn == nullptr) {
        log_message(LogLevel::Warn,
                    "MountManager: attempted to register null filesystem driver");
        return;
    }
    if (g_filesystem_driver_count >= kMaxFilesystemDrivers) {
        log_message(LogLevel::Warn,
                    "MountManager: filesystem driver registry is full");
        return;
    }
    g_filesystem_drivers[g_filesystem_driver_count++] = fn;
}

bool mount_requested_filesystems(const char* root_spec,
                                 const char* const* mount_specs,
                                 size_t mount_count,
                                 size_t& out_total_mounted) {
    ensure_builtins_registered();

    BlockDevice discovered[kMaxDiscoveredDevices];
    size_t device_count = 0;

    for (size_t i = 0; i < g_provider_count; ++i) {
        if (device_count >= kMaxDiscoveredDevices) {
            log_message(LogLevel::Warn,
                        "MountManager: device discovery limit reached");
            break;
        }

        size_t remaining = kMaxDiscoveredDevices - device_count;
        size_t added =
            g_providers[i](discovered + device_count, remaining);
        if (added > remaining) {
            added = remaining;
        }
        device_count += added;
    }

    constexpr size_t kMaxMountSpecs = 16;
    bool mount_matched[kMaxMountSpecs] = {};
    if (mount_count > kMaxMountSpecs) {
        log_message(LogLevel::Warn,
                    "MountManager: mount list truncated from %zu to %zu entries",
                    mount_count, static_cast<size_t>(kMaxMountSpecs));
        mount_count = kMaxMountSpecs;
    }

    bool root_requested = root_spec != nullptr && root_spec[0] != '\0';
    bool root_mounted = !root_requested;
    size_t mounted = 0;

    if (root_requested) {
        for (size_t i = 0; i < mount_count; ++i) {
            if (mount_specs[i] != nullptr &&
                strings_equal(root_spec, mount_specs[i])) {
                mount_matched[i] = true;
            }
        }
    }

    for (size_t device_index = 0; device_index < device_count; ++device_index) {
        const BlockDevice& device = discovered[device_index];
        const char* device_name =
            (device.name != nullptr) ? device.name : "(unnamed)";

        bool is_root = false;
        size_t mount_match = SIZE_MAX;

        if (root_requested && strings_equal(device.name, root_spec)) {
            is_root = true;
        } else {
            for (size_t i = 0; i < mount_count; ++i) {
                if (mount_matched[i]) continue;
                if (mount_specs[i] != nullptr &&
                    strings_equal(device.name, mount_specs[i])) {
                    mount_match = i;
                    break;
                }
            }
        }

        if (!is_root && mount_match == SIZE_MAX) {
            continue;
        }

        bool handled = false;
        for (size_t driver_index = 0;
             driver_index < g_filesystem_driver_count; ++driver_index) {
            if (g_filesystem_drivers[driver_index](device)) {
                handled = true;
                ++mounted;
                if (is_root) {
                    root_mounted = true;
                } else if (mount_match != SIZE_MAX) {
                    mount_matched[mount_match] = true;
                }
                break;
            }
        }

        if (!handled) {
            log_message(LogLevel::Info,
                        "MountManager: no filesystem driver accepted %s",
                        device_name);
        }
    }

    for (size_t i = 0; i < mount_count; ++i) {
        if (!mount_matched[i]) {
            log_message(LogLevel::Warn,
                        "MountManager: requested mount '%s' not found",
                        mount_specs[i]);
        }
    }

    if (root_requested && !root_mounted) {
        log_message(LogLevel::Warn,
                    "MountManager: root filesystem '%s' not found",
                    root_spec);
    }

    out_total_mounted = mounted;
    return root_mounted;
}

}  // namespace fs
