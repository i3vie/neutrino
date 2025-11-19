#include "drivers/storage/emmc_provider.hpp"

#include "drivers/fs/block_device.hpp"
#include "drivers/fs/mount_manager.hpp"
#include "drivers/log/logging.hpp"
#include "drivers/storage/emmc.hpp"

namespace {

constexpr size_t kMaxDevices = 4;
constexpr size_t kNameLength = 16;
constexpr size_t kSectorSize = 512;

struct EmmcContext {
    size_t device_index;
};

EmmcContext g_contexts[kMaxDevices];
char g_name_storage[kMaxDevices][kNameLength];

fs::BlockIoStatus emmc_block_read(void* context,
                                  uint32_t lba,
                                  uint8_t count,
                                  void* buffer) {
    if (context == nullptr || buffer == nullptr || count == 0) {
        return fs::BlockIoStatus::NoDevice;
    }
    auto* ctx = static_cast<EmmcContext*>(context);
    emmc::Status status = emmc::read_blocks(ctx->device_index, lba, count,
                                            buffer);
    switch (status) {
        case emmc::Status::Ok:
            return fs::BlockIoStatus::Ok;
        case emmc::Status::Busy:
            return fs::BlockIoStatus::Busy;
        case emmc::Status::NoDevice:
            return fs::BlockIoStatus::NoDevice;
        default:
            return fs::BlockIoStatus::IoError;
    }
}

size_t enumerate_emmc_devices(fs::BlockDevice* out_devices,
                              size_t max_devices) {
    if (out_devices == nullptr || max_devices == 0) {
        return 0;
    }

    if (!emmc::init()) {
        return 0;
    }

    size_t count = emmc::device_count();
    if (count > kMaxDevices) {
        count = kMaxDevices;
    }
    if (count > max_devices) {
        count = max_devices;
    }

    for (size_t i = 0; i < count; ++i) {
        EmmcContext& ctx = g_contexts[i];
        ctx.device_index = i;

        char* name = g_name_storage[i];
        name[0] = '\0';
        size_t len = 0;
        const char prefix[] = "EMMC_";
        while (len + 1 < kNameLength && prefix[len] != '\0') {
            name[len] = prefix[len];
            ++len;
        }
        size_t value = i;
        char digits[4];
        size_t digit_count = 0;
        do {
            digits[digit_count++] = static_cast<char>('0' + (value % 10));
            value /= 10;
        } while (value > 0 && digit_count < sizeof(digits));
        for (size_t d = 0; d < digit_count && len + 1 < kNameLength; ++d) {
            name[len++] = digits[digit_count - 1 - d];
        }
        name[len] = '\0';

        fs::BlockDevice& device = out_devices[i];
        device.name = name;
        device.sector_size = kSectorSize;
        device.sector_count = emmc::device_sector_count(i);
        device.read = emmc_block_read;
        device.write = nullptr;
        device.context = &ctx;
    }

    return count;
}

}  // namespace

namespace fs {

void register_emmc_block_device_provider() {
    register_block_device_provider(enumerate_emmc_devices);
}

}  // namespace fs
