#include "driver.hpp"
#include "lib/mem.hpp"

#include "drivers/log/logging.hpp"
#include "neufs.hpp"
#include "../mount_manager.hpp"
#include "fs/vfs.hpp"

namespace {

constexpr size_t kMaxNeufsVolumes = 8;
neufs::NeufsVolume g_volumes[kMaxNeufsVolumes];

neufs::NeufsVolume* allocate_volume() {
    for (size_t i = 0; i < kMaxNeufsVolumes; ++i) {
        if (!g_volumes[i].mounted) {
            memset(&g_volumes[i], 0, sizeof(neufs::NeufsVolume));
            return &g_volumes[i];
        }
    }
    return nullptr;
}

bool neufs_probe(const fs::BlockDevice& device) {
    log_message(LogLevel::Info,
                "NEUFS: probing %s",
                device.name != nullptr ? device.name : "(unnamed)");

    neufs::NeufsVolume* volume = allocate_volume();
    if (volume == nullptr) {
        log_message(LogLevel::Warn,
                    "NEUFS: no free volume slots to mount %s",
                    device.name != nullptr ? device.name : "(unnamed)");
        return false;
    }

    if (!neufs::neufs_mount(*volume, device)) {
        log_message(LogLevel::Info,
                    "NEUFS: %s not recognized",
                    device.name != nullptr ? device.name : "(unnamed)");
        return false;
    }

    if (device.name == nullptr || device.name[0] == '\0') {
        log_message(LogLevel::Warn,
                    "NEUFS: device without name cannot be mounted");
        volume->mounted = false;
        return false;
    }

    if (!vfs::register_mount(device.name, &neufs::neufs_vfs_ops(), volume)) {
        log_message(LogLevel::Warn,
                    "NEUFS: failed to register VFS mount for %s",
                    device.name != nullptr ? device.name : "(unnamed)");
        volume->mounted = false;
        return false;
    }

    log_message(LogLevel::Info,
                "NEUFS: mounted %s",
                device.name != nullptr ? device.name : "(unnamed)");
    return true;
}

}  // namespace

namespace fs {

void register_neufs_filesystem_driver() {
    static bool registered = false;
    if (registered) {
        return;
    }
    registered = true;
    register_filesystem_driver(neufs_probe);
}

}  // namespace fs
