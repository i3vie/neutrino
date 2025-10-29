#include "driver.hpp"

#include "drivers/log/logging.hpp"
#include "fat32.hpp"
#include "lib/mem.hpp"
#include "../mount_manager.hpp"
#include "fs/vfs.hpp"

namespace {

constexpr size_t kMaxFat32Volumes = 16;
Fat32Volume g_volumes[kMaxFat32Volumes];

Fat32Volume* allocate_volume() {
    for (size_t i = 0; i < kMaxFat32Volumes; ++i) {
        if (!g_volumes[i].mounted) {
            memset(&g_volumes[i], 0, sizeof(Fat32Volume));
            return &g_volumes[i];
        }
    }
    return nullptr;
}

bool fat32_probe(const fs::BlockDevice& device) {
    Fat32Volume* volume = allocate_volume();
    if (volume == nullptr) {
        log_message(LogLevel::Warn,
                    "FAT32: no free volume slots to mount %s",
                    device.name != nullptr ? device.name : "(unnamed)");
        return false;
    }

    if (!fat32_mount(*volume, device)) {
        return false;
    }

    if (device.name == nullptr || device.name[0] == '\0') {
        log_message(LogLevel::Warn,
                    "FAT32: device without name cannot be mounted");
        volume->mounted = false;
        return false;
    }

    if (!vfs::register_mount(device.name, &fat32_vfs_ops(), volume)) {
        log_message(LogLevel::Warn,
                    "FAT32: failed to register VFS mount for %s",
                    device.name != nullptr ? device.name : "(unnamed)");
        volume->mounted = false;
        return false;
    }

    return true;
}

}  // namespace

namespace fs {

void register_fat32_filesystem_driver() {
    static bool registered = false;
    if (registered) {
        return;
    }
    registered = true;
    register_filesystem_driver(fat32_probe);
}

}  // namespace fs
