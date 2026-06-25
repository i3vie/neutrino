#include "usb_mass_storage.hpp"

#include <stddef.h>
#include <stdint.h>

#include "drivers/fs/block_device.hpp"
#include "drivers/fs/mount_manager.hpp"
#include "drivers/log/logging.hpp"
#include "kernel/descriptor.hpp"
#include "lib/mem.hpp"

namespace {
void refresh_usb_storage_descriptors();
}

namespace usb::mass_storage {
namespace {

constexpr uint8_t kClassMassStorage = 0x08;
constexpr uint8_t kSubclassScsiTransparent = 0x06;
constexpr uint8_t kProtocolBulkOnly = 0x50;

constexpr uint32_t kCbwSignature = 0x43425355;
constexpr uint32_t kCswSignature = 0x53425355;
constexpr uint8_t kDataIn = 0x80;
constexpr uint8_t kCswPassed = 0x00;
constexpr uint8_t kRequestClearFeature = 0x01;
constexpr uint8_t kFeatureEndpointHalt = 0x00;
constexpr uint8_t kBulkOnlyReset = 0xFF;

constexpr uint8_t kScsiTestUnitReady = 0x00;
constexpr uint8_t kScsiRequestSense = 0x03;
constexpr uint8_t kScsiInquiry = 0x12;
constexpr uint8_t kScsiReadCapacity10 = 0x25;
constexpr uint8_t kScsiRead10 = 0x28;
constexpr uint8_t kScsiWrite10 = 0x2A;

constexpr size_t kMaxDevices = 8;
constexpr size_t kNameLen = 16;
constexpr size_t kInquiryLen = 36;
constexpr size_t kSenseLen = 18;
constexpr size_t kIoBounceSize = 4096;

struct [[gnu::packed]] CommandBlockWrapper {
    uint32_t signature;
    uint32_t tag;
    uint32_t transfer_length;
    uint8_t flags;
    uint8_t lun;
    uint8_t cdb_length;
    uint8_t cdb[16];
};

struct [[gnu::packed]] CommandStatusWrapper {
    uint32_t signature;
    uint32_t tag;
    uint32_t residue;
    uint8_t status;
};

struct DeviceState {
    bool used;
    usb::Device device;
    usb::Endpoint bulk_in;
    usb::Endpoint bulk_out;
    uint8_t interface_number;
    IdentifyInfo identify;
    char name[kNameLen];
    uint32_t next_tag;
    bool last_command_failed;
    volatile int lock;
    // Keep xHCI DMA away from stack and filesystem buffers.  Some real xHCI
    // controllers time out when handed virtual buffers with an unsuitable
    // physical layout even though small, page-aligned discovery reads work.
    alignas(4096) uint8_t io_bounce[kIoBounceSize];
};

DeviceState g_devices[kMaxDevices]{};
size_t g_device_count = 0;
constexpr IdentifyInfo kEmptyIdentifyInfo = {false, "", "", 0, 0, false};

void cpu_relax() {
    asm volatile("pause");
}

void lock_device(DeviceState& device) {
    while (__atomic_test_and_set(&device.lock, __ATOMIC_ACQUIRE)) {
        cpu_relax();
    }
}

void unlock_device(DeviceState& device) {
    __atomic_clear(&device.lock, __ATOMIC_RELEASE);
}

uint32_t read_be32(const uint8_t* data) {
    return (static_cast<uint32_t>(data[0]) << 24) |
           (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) |
           static_cast<uint32_t>(data[3]);
}

void store_be32(uint8_t* data, uint32_t value) {
    data[0] = static_cast<uint8_t>((value >> 24) & 0xFFu);
    data[1] = static_cast<uint8_t>((value >> 16) & 0xFFu);
    data[2] = static_cast<uint8_t>((value >> 8) & 0xFFu);
    data[3] = static_cast<uint8_t>(value & 0xFFu);
}

size_t copy_padded_ascii(char* dest,
                         size_t dest_size,
                         const uint8_t* src,
                         size_t src_size) {
    if (dest == nullptr || dest_size == 0) {
        return 0;
    }
    size_t out = 0;
    for (size_t i = 0; i < src_size && out + 1 < dest_size; ++i) {
        char c = static_cast<char>(src[i]);
        if (c < 0x20 || c > 0x7E) {
            c = ' ';
        }
        dest[out++] = c;
    }
    while (out > 0 && dest[out - 1] == ' ') {
        --out;
    }
    dest[out] = '\0';
    return out;
}

bool append_decimal(char* buffer, size_t buffer_size, size_t value) {
    size_t len = 0;
    while (len < buffer_size && buffer[len] != '\0') {
        ++len;
    }
    if (len >= buffer_size) {
        return false;
    }
    char digits[20];
    size_t digit_count = 0;
    do {
        digits[digit_count++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    } while (value != 0 && digit_count < sizeof(digits));
    if (len + digit_count >= buffer_size) {
        return false;
    }
    for (size_t i = 0; i < digit_count; ++i) {
        buffer[len++] = digits[digit_count - 1 - i];
    }
    buffer[len] = '\0';
    return true;
}

Status transfer_status_to_status(usb::TransferStatus status) {
    switch (status) {
        case usb::TransferStatus::Ok:
            return Status::Ok;
        case usb::TransferStatus::Timeout:
            return Status::Timeout;
        case usb::TransferStatus::NoDevice:
            return Status::NoDevice;
        case usb::TransferStatus::Unsupported:
            return Status::Unsupported;
        default:
            return Status::IoError;
    }
}

bool find_bulk_endpoints(const usb::Device& device,
                         usb::Endpoint& bulk_in,
                         usb::Endpoint& bulk_out) {
    bool found_in = false;
    bool found_out = false;
    for (size_t i = 0; i < device.endpoint_count; ++i) {
        const usb::Endpoint& endpoint = device.endpoints[i];
        if (endpoint.type != usb::EndpointType::Bulk) {
            continue;
        }
        if ((endpoint.address & 0x80u) != 0) {
            bulk_in = endpoint;
            found_in = true;
        } else {
            bulk_out = endpoint;
            found_out = true;
        }
    }
    return found_in && found_out;
}

Status bulk_transfer(DeviceState& state,
                     uint8_t endpoint,
                     void* data,
                     size_t length) {
    if (state.device.transport.bulk == nullptr) {
        return Status::Unsupported;
    }
    size_t transferred = 0;
    usb::TransferStatus status = state.device.transport.bulk(
        state.device.transport.context, endpoint, data, length, transferred);
    if (status != usb::TransferStatus::Ok) {
        return transfer_status_to_status(status);
    }
    return transferred == length ? Status::Ok : Status::IoError;
}

bool control_request(DeviceState& state, const usb::ControlRequest& request) {
    if (state.device.transport.control == nullptr) {
        return false;
    }
    return state.device.transport.control(state.device.transport.context,
                                          request, nullptr) ==
           usb::TransferStatus::Ok;
}

bool clear_endpoint_halt(DeviceState& state, uint8_t endpoint) {
    usb::ControlRequest clear_halt{
        0x02,  // host-to-device, standard, endpoint
        kRequestClearFeature,
        kFeatureEndpointHalt,
        endpoint,
        0,
    };
    if (!control_request(state, clear_halt)) {
        return false;
    }
    if (state.device.transport.reset_endpoint == nullptr) {
        return false;
    }
    return state.device.transport.reset_endpoint(
               state.device.transport.context, endpoint) ==
           usb::TransferStatus::Ok;
}

bool bot_reset_recovery(DeviceState& state) {
    usb::ControlRequest reset{
        0x21,  // host-to-device, class, interface
        kBulkOnlyReset,
        0,
        state.interface_number,
        0,
    };
    bool reset_ok = control_request(state, reset);
    bool in_ok = clear_endpoint_halt(state, state.bulk_in.address);
    bool out_ok = clear_endpoint_halt(state, state.bulk_out.address);
    if (!reset_ok || !in_ok || !out_ok) {
        log_message(LogLevel::Warn,
                    "usb-storage %s: BOT recovery failed reset=%u in=%u out=%u",
                    state.name,
                    reset_ok ? 1u : 0u,
                    in_ok ? 1u : 0u,
                    out_ok ? 1u : 0u);
        return false;
    }
    return true;
}

Status bot_command(DeviceState& state,
                   const uint8_t* cdb,
                   uint8_t cdb_length,
                   void* data,
                   uint32_t data_length,
                   bool data_in) {
    if (cdb == nullptr || cdb_length == 0 || cdb_length > 16) {
        return Status::IoError;
    }
    if (data_length != 0 && data == nullptr) {
        return Status::IoError;
    }
    state.last_command_failed = false;

    CommandBlockWrapper cbw{};
    cbw.signature = kCbwSignature;
    cbw.tag = state.next_tag++;
    cbw.transfer_length = data_length;
    cbw.flags = data_in ? kDataIn : 0;
    cbw.lun = 0;
    cbw.cdb_length = cdb_length;
    memcpy(cbw.cdb, cdb, cdb_length);

    Status status = bulk_transfer(
        state, state.bulk_out.address, &cbw, sizeof(cbw));
    if (status != Status::Ok) {
        log_message(LogLevel::Warn,
                    "usb-storage %s: CBW transfer failed status=%d opcode=%02x",
                    state.name, static_cast<int>(status), cdb[0]);
        return status;
    }

    if (data_length != 0) {
        status = bulk_transfer(state,
                               data_in ? state.bulk_in.address
                                       : state.bulk_out.address,
                               data,
                               data_length);
        if (status != Status::Ok) {
            log_message(LogLevel::Warn,
                        "usb-storage %s: data transfer failed status=%d opcode=%02x length=%u",
                        state.name, static_cast<int>(status), cdb[0],
                        static_cast<unsigned int>(data_length));
            return status;
        }
    }

    CommandStatusWrapper csw{};
    status = bulk_transfer(state, state.bulk_in.address, &csw, sizeof(csw));
    if (status != Status::Ok) {
        log_message(LogLevel::Warn,
                    "usb-storage %s: CSW transfer failed status=%d opcode=%02x",
                    state.name, static_cast<int>(status), cdb[0]);
        return status;
    }
    if (csw.signature != kCswSignature || csw.tag != cbw.tag) {
        log_message(LogLevel::Warn,
                    "usb-storage %s: invalid CSW signature=%08x tag=%u expected=%u",
                    state.name, csw.signature, csw.tag, cbw.tag);
        return Status::IoError;
    }
    if (csw.status != kCswPassed) {
        state.last_command_failed = true;
        log_message(LogLevel::Warn,
                    "usb-storage %s: command failed opcode=%02x csw_status=%u residue=%u",
                    state.name, cdb[0], csw.status, csw.residue);
        return Status::IoError;
    }
    return Status::Ok;
}

void request_sense(DeviceState& state) {
    uint8_t cdb[6]{};
    uint8_t sense[kSenseLen]{};
    cdb[0] = kScsiRequestSense;
    cdb[4] = sizeof(sense);
    if (bot_command(state, cdb, 6, sense, sizeof(sense), true) != Status::Ok) {
        return;
    }
    uint8_t response_code = sense[0] & 0x7Fu;
    if (response_code == 0x70 || response_code == 0x71) {
        log_message(LogLevel::Warn,
                    "usb-storage %s: sense key=%02x asc=%02x ascq=%02x",
                    state.name, sense[2] & 0x0Fu, sense[12], sense[13]);
    }
}

Status test_unit_ready(DeviceState& state) {
    uint8_t cdb[6]{};
    cdb[0] = kScsiTestUnitReady;
    return bot_command(state, cdb, 6, nullptr, 0, true);
}

Status inquiry(DeviceState& state) {
    uint8_t cdb[6]{};
    uint8_t data[kInquiryLen]{};
    cdb[0] = kScsiInquiry;
    cdb[4] = sizeof(data);
    Status status = bot_command(state, cdb, 6, data, sizeof(data), true);
    if (status != Status::Ok) {
        return status;
    }
    state.identify.removable = (data[1] & 0x80u) != 0;
    copy_padded_ascii(state.identify.vendor,
                      sizeof(state.identify.vendor),
                      data + 8,
                      8);
    copy_padded_ascii(state.identify.product,
                      sizeof(state.identify.product),
                      data + 16,
                      16);
    return Status::Ok;
}

Status read_capacity(DeviceState& state) {
    uint8_t cdb[10]{};
    uint8_t data[8]{};
    cdb[0] = kScsiReadCapacity10;
    Status status = bot_command(state, cdb, 10, data, sizeof(data), true);
    if (status != Status::Ok) {
        return status;
    }
    uint32_t last_lba = read_be32(data);
    uint32_t block_len = read_be32(data + 4);
    if (block_len == 0) {
        return Status::IoError;
    }
    state.identify.sector_count = static_cast<uint64_t>(last_lba) + 1;
    state.identify.sector_size = block_len;
    state.identify.present = true;
    return Status::Ok;
}

Status initialize_device(DeviceState& state) {
    if (state.device.transport.bulk == nullptr) {
        log_message(LogLevel::Warn,
                    "usb-storage: device addr=%u has no bulk transport",
                    static_cast<unsigned int>(state.device.address));
        return Status::Unsupported;
    }

    Status status = inquiry(state);
    if (status != Status::Ok) {
        return status;
    }

    for (size_t attempt = 0; attempt < 4; ++attempt) {
        status = test_unit_ready(state);
        if (status == Status::Ok) {
            break;
        }
        request_sense(state);
    }
    if (status != Status::Ok) {
        return status;
    }

    return read_capacity(state);
}

}  // namespace

void init() {
    (void)usb::register_class_driver("usb-storage", probe_device);
}

bool probe_device(const usb::Device& device) {
    bool mass_storage = false;
    uint8_t interface_number = 0;
    for (size_t i = 0; i < device.interface_count; ++i) {
        const usb::Interface& interface = device.interfaces[i];
        if (interface.class_code == kClassMassStorage &&
            interface.subclass == kSubclassScsiTransparent &&
            interface.protocol == kProtocolBulkOnly) {
            mass_storage = true;
            interface_number = interface.number;
            break;
        }
    }
    if (!mass_storage) {
        return false;
    }

    if (g_device_count >= kMaxDevices) {
        log_message(LogLevel::Warn, "usb-storage: device table full");
        return false;
    }

    DeviceState& state = g_devices[g_device_count];
    state = {};
    state.device = device;
    state.interface_number = interface_number;
    state.next_tag = 1;
    state.lock = 0;
    if (!find_bulk_endpoints(device, state.bulk_in, state.bulk_out)) {
        log_message(LogLevel::Warn,
                    "usb-storage: mass-storage interface lacks bulk IN/OUT endpoints");
        return false;
    }

    state.name[0] = 'U';
    state.name[1] = 'S';
    state.name[2] = 'B';
    state.name[3] = 'M';
    state.name[4] = 'S';
    state.name[5] = '_';
    state.name[6] = '\0';
    if (!append_decimal(state.name, sizeof(state.name), g_device_count)) {
        return false;
    }

    Status status = Status::IoError;
    for (size_t attempt = 0; attempt < 3; ++attempt) {
        status = initialize_device(state);
        if (status == Status::Ok) {
            break;
        }
        (void)bot_reset_recovery(state);
    }
    if (status != Status::Ok) {
        log_message(LogLevel::Warn,
                    "usb-storage: failed to initialize addr=%u status=%d",
                    static_cast<unsigned int>(device.address),
                    static_cast<int>(status));
        state = {};
        return false;
    }

    state.used = true;
    ++g_device_count;
    log_message(LogLevel::Info,
                "usb-storage: %s %s %s sectors=%llu sector_size=%u",
                state.name,
                state.identify.vendor[0] != '\0' ? state.identify.vendor
                                                  : "USB",
                state.identify.product[0] != '\0' ? state.identify.product
                                                   : "storage",
                static_cast<unsigned long long>(state.identify.sector_count),
                static_cast<unsigned int>(state.identify.sector_size));
    refresh_usb_storage_descriptors();
    return true;
}

size_t device_count() {
    return g_device_count;
}

const IdentifyInfo& identify(size_t device_index) {
    if (device_index >= g_device_count || !g_devices[device_index].used) {
        return kEmptyIdentifyInfo;
    }
    return g_devices[device_index].identify;
}

const char* device_name(size_t device_index) {
    if (device_index >= g_device_count || !g_devices[device_index].used) {
        return "";
    }
    return g_devices[device_index].name;
}

Status read_sectors(size_t device_index,
                    uint64_t lba,
                    uint8_t sector_count,
                    void* buffer) {
    if (device_index >= g_device_count || buffer == nullptr ||
        sector_count == 0) {
        return Status::NoDevice;
    }
    DeviceState& state = g_devices[device_index];
    if (!state.used || !state.identify.present ||
        state.identify.sector_size == 0 ||
        state.identify.sector_size > kIoBounceSize) {
        return Status::NoDevice;
    }
    if (lba > 0xFFFFFFFFull ||
        lba + sector_count > state.identify.sector_count) {
        return Status::IoError;
    }

    lock_device(state);
    Status status = Status::Ok;
    auto* out = static_cast<uint8_t*>(buffer);
    uint8_t remaining = sector_count;
    uint64_t current_lba = lba;
    uint8_t max_chunk = static_cast<uint8_t>(
        kIoBounceSize / state.identify.sector_size);
    while (remaining != 0) {
        uint8_t chunk = remaining < max_chunk ? remaining : max_chunk;
        uint32_t byte_count =
            static_cast<uint32_t>(chunk) * state.identify.sector_size;
        uint8_t cdb[10]{};
        cdb[0] = kScsiRead10;
        store_be32(cdb + 2, static_cast<uint32_t>(current_lba));
        cdb[7] = 0;
        cdb[8] = chunk;

        status = bot_command(state, cdb, 10, state.io_bounce,
                             byte_count, true);
        if (status != Status::Ok && state.last_command_failed) {
            request_sense(state);
        }
        if (status != Status::Ok && bot_reset_recovery(state)) {
            log_message(LogLevel::Info,
                        "usb-storage %s: retrying READ(10) lba=%llu count=%u after BOT recovery",
                        state.name,
                        static_cast<unsigned long long>(current_lba),
                        static_cast<unsigned int>(chunk));
            status = bot_command(state, cdb, 10, state.io_bounce,
                                 byte_count, true);
        }
        if (status != Status::Ok) {
            break;
        }
        memcpy(out, state.io_bounce, byte_count);
        out += byte_count;
        current_lba += chunk;
        remaining = static_cast<uint8_t>(remaining - chunk);
    }
    unlock_device(state);
    return status;
}

Status write_sectors(size_t device_index,
                     uint64_t lba,
                     uint8_t sector_count,
                     const void* buffer) {
    if (device_index >= g_device_count || buffer == nullptr ||
        sector_count == 0) {
        return Status::NoDevice;
    }
    DeviceState& state = g_devices[device_index];
    if (!state.used || !state.identify.present ||
        state.identify.sector_size == 0 ||
        state.identify.sector_size > kIoBounceSize) {
        return Status::NoDevice;
    }
    if (lba > 0xFFFFFFFFull ||
        lba + sector_count > state.identify.sector_count) {
        return Status::IoError;
    }

    lock_device(state);
    Status status = Status::Ok;
    const auto* in = static_cast<const uint8_t*>(buffer);
    uint8_t remaining = sector_count;
    uint64_t current_lba = lba;
    uint8_t max_chunk = static_cast<uint8_t>(
        kIoBounceSize / state.identify.sector_size);
    while (remaining != 0) {
        uint8_t chunk = remaining < max_chunk ? remaining : max_chunk;
        uint32_t byte_count =
            static_cast<uint32_t>(chunk) * state.identify.sector_size;
        memcpy(state.io_bounce, in, byte_count);

        uint8_t cdb[10]{};
        cdb[0] = kScsiWrite10;
        store_be32(cdb + 2, static_cast<uint32_t>(current_lba));
        cdb[7] = 0;
        cdb[8] = chunk;
        status = bot_command(state, cdb, 10, state.io_bounce,
                             byte_count, false);
        if (status != Status::Ok && state.last_command_failed) {
            request_sense(state);
        }
        if (status != Status::Ok && bot_reset_recovery(state)) {
            log_message(LogLevel::Info,
                        "usb-storage %s: retrying WRITE(10) lba=%llu count=%u after BOT recovery",
                        state.name,
                        static_cast<unsigned long long>(current_lba),
                        static_cast<unsigned int>(chunk));
            status = bot_command(state, cdb, 10, state.io_bounce,
                                 byte_count, false);
        }
        if (status != Status::Ok) {
            break;
        }
        in += byte_count;
        current_lba += chunk;
        remaining = static_cast<uint8_t>(remaining - chunk);
    }
    unlock_device(state);
    return status;
}

}  // namespace usb::mass_storage

namespace {

struct PartitionInfo {
    uint8_t type;
    uint8_t ordinal;
    uint64_t start_lba;
    uint64_t sector_count;
};

struct UsbStoragePartitionContext {
    size_t device_index;
    uint64_t lba_base;
};

constexpr size_t kMaxUsbStorageDevices = 8;
constexpr size_t kMaxPartitionsPerDevice = 8;
constexpr size_t kMaxExportedDevices =
    kMaxUsbStorageDevices * (kMaxPartitionsPerDevice + 1);
constexpr size_t kProviderNameLen = 24;

alignas(4096) uint8_t g_partition_buffer[4096];
UsbStoragePartitionContext g_partition_contexts[kMaxExportedDevices];
char g_name_storage[kMaxExportedDevices][kProviderNameLen];

uint32_t read_u32_le(const uint8_t* data) {
    return static_cast<uint32_t>(data[0]) |
           (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) |
           (static_cast<uint32_t>(data[3]) << 24);
}

uint64_t read_u64_le(const uint8_t* data) {
    return static_cast<uint64_t>(read_u32_le(data)) |
           (static_cast<uint64_t>(read_u32_le(data + 4)) << 32);
}

size_t copy_string(char* dest, size_t dest_size, const char* src) {
    if (dest == nullptr || dest_size == 0) {
        return 0;
    }
    size_t idx = 0;
    while (idx + 1 < dest_size && src != nullptr && src[idx] != '\0') {
        dest[idx] = src[idx];
        ++idx;
    }
    dest[idx] = '\0';
    return idx;
}

bool append_suffix(char* buffer, size_t buffer_size, uint32_t value) {
    size_t len = 0;
    while (len < buffer_size && buffer[len] != '\0') {
        ++len;
    }
    if (len + 2 >= buffer_size) {
        return false;
    }
    buffer[len++] = '_';

    char digits[10];
    size_t digit_count = 0;
    do {
        digits[digit_count++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    } while (value > 0 && digit_count < sizeof(digits));

    if (len + digit_count >= buffer_size) {
        return false;
    }
    for (size_t i = 0; i < digit_count; ++i) {
        buffer[len + i] = digits[digit_count - 1 - i];
    }
    buffer[len + digit_count] = '\0';
    return true;
}

fs::BlockIoStatus usb_storage_read(void* context,
                                   uint32_t lba,
                                   uint8_t count,
                                   void* buffer) {
    auto* ctx = static_cast<UsbStoragePartitionContext*>(context);
    usb::mass_storage::Status status = usb::mass_storage::read_sectors(
        ctx->device_index, ctx->lba_base + lba, count, buffer);
    switch (status) {
        case usb::mass_storage::Status::Ok:
            return fs::BlockIoStatus::Ok;
        case usb::mass_storage::Status::Busy:
            return fs::BlockIoStatus::Busy;
        case usb::mass_storage::Status::NoDevice:
            return fs::BlockIoStatus::NoDevice;
        default:
            return fs::BlockIoStatus::IoError;
    }
}

fs::BlockIoStatus usb_storage_write(void* context,
                                    uint32_t lba,
                                    uint8_t count,
                                    const void* buffer) {
    auto* ctx = static_cast<UsbStoragePartitionContext*>(context);
    usb::mass_storage::Status status = usb::mass_storage::write_sectors(
        ctx->device_index, ctx->lba_base + lba, count, buffer);
    switch (status) {
        case usb::mass_storage::Status::Ok:
            return fs::BlockIoStatus::Ok;
        case usb::mass_storage::Status::Busy:
            return fs::BlockIoStatus::Busy;
        case usb::mass_storage::Status::NoDevice:
            return fs::BlockIoStatus::NoDevice;
        default:
            return fs::BlockIoStatus::IoError;
    }
}

size_t scan_mbr_partitions(size_t device_index,
                           uint32_t sector_size,
                           PartitionInfo* partitions,
                           size_t max_partitions) {
    if (partitions == nullptr || max_partitions == 0) {
        return 0;
    }
    if (sector_size < 512 || sector_size > sizeof(g_partition_buffer)) {
        return 0;
    }
    if (usb::mass_storage::read_sectors(device_index,
                                        0,
                                        1,
                                        g_partition_buffer) !=
        usb::mass_storage::Status::Ok) {
        return 0;
    }
    if (g_partition_buffer[510] != 0x55 || g_partition_buffer[511] != 0xAA) {
        return 0;
    }

    size_t count = 0;
    for (size_t entry = 0; entry < 4 && count < max_partitions; ++entry) {
        const uint8_t* record = g_partition_buffer + 446 + (entry * 16);
        uint8_t type = record[4];
        uint32_t start_lba = read_u32_le(record + 8);
        uint32_t sectors = read_u32_le(record + 12);
        if (type == 0 || sectors == 0) {
            continue;
        }
        partitions[count++] = {
            type,
            static_cast<uint8_t>(entry),
            start_lba,
            sectors,
        };
    }
    return count;
}

bool guid_is_zero(const uint8_t* guid) {
    for (size_t i = 0; i < 16; ++i) {
        if (guid[i] != 0) {
            return false;
        }
    }
    return true;
}

size_t scan_gpt_partitions(size_t device_index,
                           uint64_t disk_sectors,
                           uint32_t sector_size,
                           PartitionInfo* partitions,
                           size_t max_partitions) {
    if (partitions == nullptr || max_partitions == 0 || disk_sectors < 34) {
        return 0;
    }
    if (sector_size < 512 || sector_size > sizeof(g_partition_buffer)) {
        return 0;
    }
    if (usb::mass_storage::read_sectors(device_index,
                                        1,
                                        1,
                                        g_partition_buffer) !=
        usb::mass_storage::Status::Ok) {
        return 0;
    }
    const uint8_t signature[8] = {'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T'};
    if (memcmp(g_partition_buffer, signature, sizeof(signature)) != 0) {
        return 0;
    }

    uint64_t entries_lba = read_u64_le(g_partition_buffer + 72);
    uint32_t entry_count = read_u32_le(g_partition_buffer + 80);
    uint32_t entry_size = read_u32_le(g_partition_buffer + 84);
    if (entries_lba == 0 || entry_size < 56 || entry_size > 512) {
        return 0;
    }

    size_t count = 0;
    uint64_t current_lba = entries_lba;
    uint32_t entry = 0;
    while (entry < entry_count && count < max_partitions) {
        if (current_lba >= disk_sectors) {
            break;
        }
        if (usb::mass_storage::read_sectors(device_index,
                                            current_lba,
                                            1,
                                            g_partition_buffer) !=
            usb::mass_storage::Status::Ok) {
            break;
        }
        size_t entries_per_sector = sector_size / entry_size;
        if (entries_per_sector == 0) {
            break;
        }
        for (size_t i = 0; i < entries_per_sector && entry < entry_count &&
                           count < max_partitions;
             ++i, ++entry) {
            const uint8_t* record =
                g_partition_buffer + (static_cast<size_t>(entry_size) * i);
            if (guid_is_zero(record)) {
                continue;
            }
            uint64_t first_lba = read_u64_le(record + 32);
            uint64_t last_lba = read_u64_le(record + 40);
            if (first_lba == 0 || last_lba < first_lba ||
                last_lba >= disk_sectors) {
                continue;
            }
            partitions[count++] = {
                0xEE,
                static_cast<uint8_t>(entry),
                first_lba,
                last_lba - first_lba + 1,
            };
        }
        ++current_lba;
    }
    return count;
}

size_t scan_partitions(size_t device_index,
                       uint64_t disk_sectors,
                       uint32_t sector_size,
                       PartitionInfo* partitions,
                       size_t max_partitions) {
    size_t gpt_count =
        scan_gpt_partitions(device_index,
                            disk_sectors,
                            sector_size,
                            partitions,
                            max_partitions);
    if (gpt_count != 0) {
        return gpt_count;
    }
    return scan_mbr_partitions(device_index,
                               sector_size,
                               partitions,
                               max_partitions);
}

size_t enumerate_usb_storage_devices(fs::BlockDevice* out_devices,
                                     size_t max_devices) {
    if (out_devices == nullptr || max_devices == 0) {
        return 0;
    }

    size_t exported_count = 0;
    size_t controller_device_count = usb::mass_storage::device_count();
    for (size_t device_index = 0; device_index < controller_device_count;
         ++device_index) {
        if (exported_count >= max_devices) {
            break;
        }

        const usb::mass_storage::IdentifyInfo& identify =
            usb::mass_storage::identify(device_index);
        if (!identify.present || identify.sector_count == 0 ||
            identify.sector_size == 0) {
            continue;
        }

        UsbStoragePartitionContext& disk_context =
            g_partition_contexts[exported_count];
        disk_context.device_index = device_index;
        disk_context.lba_base = 0;

        fs::BlockDevice& disk = out_devices[exported_count];
        disk.name = usb::mass_storage::device_name(device_index);
        disk.parent_name = nullptr;
        disk.sector_size = identify.sector_size;
        disk.sector_count = identify.sector_count;
        disk.start_lba = 0;
        disk.partition_index = 0xFFFFFFFFu;
        disk.partition_type = 0xFF;
        disk.removable = true;
        disk.descriptor_handle = descriptor::kInvalidHandle;
        disk.read = usb_storage_read;
        disk.write = usb_storage_write;
        disk.context = &disk_context;
        ++exported_count;

        PartitionInfo partitions[kMaxPartitionsPerDevice]{};
        size_t partition_count =
            scan_partitions(device_index,
                            identify.sector_count,
                            identify.sector_size,
                            partitions,
                            kMaxPartitionsPerDevice);
        if (partition_count == 0) {
            log_message(LogLevel::Info,
                        "usb-storage %s: no recognized partitions detected",
                        usb::mass_storage::device_name(device_index));
        }

        for (size_t partition_index = 0; partition_index < partition_count;
             ++partition_index) {
            if (exported_count >= max_devices) {
                log_message(LogLevel::Warn,
                            "usb-storage provider: device list exhausted");
                break;
            }

            UsbStoragePartitionContext& context =
                g_partition_contexts[exported_count];
            context.device_index = device_index;
            context.lba_base = partitions[partition_index].start_lba;

            char* name_buffer = g_name_storage[exported_count];
            copy_string(name_buffer,
                        kProviderNameLen,
                        usb::mass_storage::device_name(device_index));
            if (!append_suffix(name_buffer,
                               kProviderNameLen,
                               partitions[partition_index].ordinal)) {
                log_message(LogLevel::Warn,
                            "usb-storage provider: mount name overflow");
                continue;
            }

            fs::BlockDevice& device = out_devices[exported_count];
            device.name = name_buffer;
            device.parent_name = usb::mass_storage::device_name(device_index);
            device.sector_size = identify.sector_size;
            device.sector_count = partitions[partition_index].sector_count;
            device.start_lba = partitions[partition_index].start_lba;
            device.partition_index = partitions[partition_index].ordinal;
            device.partition_type = partitions[partition_index].type;
            device.removable = true;
            device.descriptor_handle = descriptor::kInvalidHandle;
            device.read = usb_storage_read;
            device.write = usb_storage_write;
            device.context = &context;
            ++exported_count;
        }
    }

    return exported_count;
}

void refresh_usb_storage_descriptors() {
    fs::BlockDevice devices[kMaxExportedDevices]{};
    size_t count = enumerate_usb_storage_devices(devices,
                                                 kMaxExportedDevices);
    for (size_t i = 0; i < count; ++i) {
        if (!descriptor::register_block_device(devices[i], false)) {
            log_message(LogLevel::Warn,
                        "usb-storage: failed to publish block device %s",
                        devices[i].name != nullptr ? devices[i].name
                                                   : "(unnamed)");
        }
    }
}

}  // namespace

namespace fs {

void register_usb_mass_storage_block_device_provider() {
    static bool registered = false;
    if (registered) {
        return;
    }
    registered = true;
    register_block_device_provider(enumerate_usb_storage_devices);
}

}  // namespace fs
