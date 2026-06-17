#include "../descriptor.hpp"

#include "../../drivers/fs/block_device.hpp"
#include "../../drivers/log/logging.hpp"
#include "../string_util.hpp"
#include "../scheduler.hpp"
#include "../vm.hpp"
#include "../../lib/mem.hpp"

namespace descriptor {

namespace block_device_descriptor {

constexpr size_t kMaxBlockDescriptors = 32;
constexpr size_t kMaxBlockDeviceNameLen = 32;
constexpr size_t kBlockIoBufferSize = 128 * 1024;

struct BlockDeviceRecord {
    fs::BlockDevice device;
    uint32_t handle;
    bool locked;
    bool in_use;
    char name[kMaxBlockDeviceNameLen];
};

BlockDeviceRecord g_block_devices[kMaxBlockDescriptors];

BlockDeviceRecord* find_block_device_by_name(const char* name) {
    if (name == nullptr) {
        return nullptr;
    }
    for (auto& record : g_block_devices) {
        if (!record.in_use) {
            continue;
        }
        if (string_util::equals(record.name, name)) {
            return &record;
        }
    }
    return nullptr;
}

BlockDeviceRecord* find_block_device_by_index(uint64_t index) {
    size_t count = 0;
    for (auto& record : g_block_devices) {
        if (!record.in_use) {
            continue;
        }
        if (count == index) {
            return &record;
        }
        ++count;
    }
    return nullptr;
}

struct BlockRequest {
    BlockDeviceRecord* record;
    process::Process* proc;
    DescriptorEntry* entry;
    uint64_t user_address;
    uint64_t length;
    uint64_t offset;
    uint64_t progress;
    bool is_write;
    bool in_use;
};

constexpr size_t kMaxBlockRequests = 16;
BlockRequest g_block_requests[kMaxBlockRequests];
alignas(4096) uint8_t g_block_io_buffer[kBlockIoBufferSize];
alignas(4096) uint8_t g_sync_block_io_buffer[kBlockIoBufferSize];
volatile int g_request_lock = 0;
volatile int g_sync_io_lock = 0;
bool g_worker_started = false;
process::Process* g_worker = nullptr;

void lock_requests() {
    while (__atomic_test_and_set(&g_request_lock, __ATOMIC_ACQUIRE)) {
        asm volatile("pause");
    }
}

void unlock_requests() {
    __atomic_clear(&g_request_lock, __ATOMIC_RELEASE);
}

void lock_sync_io() {
    while (__atomic_test_and_set(&g_sync_io_lock, __ATOMIC_ACQUIRE)) {
        asm volatile("pause");
    }
}

void unlock_sync_io() {
    __atomic_clear(&g_sync_io_lock, __ATOMIC_RELEASE);
}

bool copy_from_caller(const process::Process& proc,
                      void* dest,
                      uint64_t src,
                      size_t length) {
    if (length == 0) {
        return true;
    }
    if (dest == nullptr || src == 0) {
        return false;
    }
    if (is_kernel_process(proc)) {
        memcpy(dest, reinterpret_cast<const void*>(src), length);
        return true;
    }
    return vm::copy_from_user(proc.cr3, dest, src, length);
}

bool copy_to_caller(const process::Process& proc,
                    uint64_t dest,
                    const void* src,
                    size_t length) {
    if (length == 0) {
        return true;
    }
    if (dest == 0 || src == nullptr) {
        return false;
    }
    if (is_kernel_process(proc)) {
        memcpy(reinterpret_cast<void*>(dest), src, length);
        return true;
    }
    return vm::copy_to_user(proc.cr3, dest, src, length);
}

BlockRequest* allocate_request() {
    for (auto& req : g_block_requests) {
        if (!req.in_use) {
            req = {};
            req.in_use = true;
            return &req;
        }
    }
    return nullptr;
}

void complete_request(BlockRequest& req, int64_t result) {
    if (req.proc != nullptr) {
        req.proc->context.rax = static_cast<uint64_t>(result);
        req.proc->state = process::State::Ready;
        req.proc->waiting_on = nullptr;
        scheduler::enqueue(req.proc);
    }
    req.in_use = false;
}

bool enqueue_block_request(BlockDeviceRecord& record,
                           process::Process& proc,
                           DescriptorEntry& entry,
                           uint64_t user_address,
                           uint64_t length,
                           uint64_t offset,
                           bool is_write) {
    BlockRequest* req = allocate_request();
    if (req == nullptr) {
        return false;
    }
    req->record = &record;
    req->proc = &proc;
    req->entry = &entry;
    req->user_address = user_address;
    req->length = length;
    req->offset = offset;
    req->progress = 0;
    req->is_write = is_write;

    proc.state = process::State::Blocked;
    proc.waiting_on = req;
    // Wake the worker if it's sleeping.
    if (g_worker != nullptr && g_worker->state == process::State::Blocked) {
        g_worker->state = process::State::Ready;
        g_worker->waiting_on = nullptr;
        scheduler::enqueue(g_worker);
    }
    return true;
}

void service_request(BlockRequest& req, size_t& sectors_processed, size_t budget) {
    auto* record = req.record;
    auto* proc = req.proc;
    if (record == nullptr || proc == nullptr || !record->in_use) {
        complete_request(req, -1);
        return;
    }
    uint64_t sector_size = record->device.sector_size;
    if (sector_size == 0) {
        complete_request(req, -1);
        return;
    }

    if (req.progress >= req.length) {
        complete_request(req, static_cast<int64_t>(req.length));
        return;
    }

    while (sectors_processed < budget && req.progress < req.length) {
        uint64_t remaining = req.length - req.progress;
        uint64_t cur_offset = req.offset + req.progress;
        if ((cur_offset % sector_size) != 0) {
            complete_request(req, -1);
            return;
        }
        uint64_t lba = cur_offset / sector_size;
        uint64_t sectors_left =
            (remaining + sector_size - 1) / sector_size;
        size_t max_sectors =
            static_cast<size_t>(sizeof(g_block_io_buffer) / sector_size);
        size_t to_do = sectors_left < budget - sectors_processed
                           ? static_cast<size_t>(sectors_left)
                           : budget - sectors_processed;
        if (to_do > max_sectors) {
            to_do = max_sectors;
        }
        if (to_do == 0) {
            break;
        }
        size_t transfer_bytes = to_do * static_cast<size_t>(sector_size);
        uint8_t* buffer = g_block_io_buffer;

        if (req.is_write) {
            if (!vm::copy_from_user(proc->cr3,
                                    buffer,
                                    req.user_address + req.progress,
                                    transfer_bytes)) {
                complete_request(req, -1);
                return;
            }
            fs::BlockIoStatus status = fs::block_write(record->device,
                                                       static_cast<uint32_t>(lba),
                                                       static_cast<uint8_t>(to_do),
                                                       buffer);
            if (status == fs::BlockIoStatus::Busy) {
                return;
            }
            if (status != fs::BlockIoStatus::Ok) {
                complete_request(req, -1);
                return;
            }
        } else {
            fs::BlockIoStatus status = fs::block_read(record->device,
                                                      static_cast<uint32_t>(lba),
                                                      static_cast<uint8_t>(to_do),
                                                      buffer);
            if (status == fs::BlockIoStatus::Busy) {
                return;
            }
            if (status != fs::BlockIoStatus::Ok) {
                complete_request(req, -1);
                return;
            }
            if (!vm::copy_to_user(proc->cr3,
                                  req.user_address + req.progress,
                                  buffer,
                                  transfer_bytes)) {
                complete_request(req, -1);
                return;
            }
        }

        req.progress += transfer_bytes;
        sectors_processed += to_do;

        if (req.progress >= req.length) {
            complete_request(req, static_cast<int64_t>(req.progress));
            return;
        }
    }
}

int64_t block_device_read(process::Process& proc,
                          DescriptorEntry& entry,
                          uint64_t user_address,
                          uint64_t length,
                          uint64_t offset) {
    auto* record = static_cast<BlockDeviceRecord*>(entry.object);
    if (record == nullptr || !record->in_use) {
        return -1;
    }
    if (record->locked && !is_kernel_process(proc)) {
        return -1;
    }
    if (length == 0) {
        return 0;
    }
    uint64_t sector_size = record->device.sector_size;
    if (sector_size == 0) {
        return -1;
    }
    if ((offset % sector_size) != 0) {
        return -1;
    }
    if ((length % sector_size) != 0) {
        return -1;
    }
    uint64_t sector_count = length / sector_size;
    if (sector_count == 0) {
        return -1;
    }
    uint64_t lba = offset / sector_size;
    if (lba >= record->device.sector_count) {
        return -1;
    }
    if (lba + sector_count > record->device.sector_count) {
        return -1;
    }

    auto read_fn = record->device.read;
    if (read_fn == nullptr) {
        return -1;
    }
    uint64_t remaining = sector_count;
    uint64_t current_lba = lba;
    if (user_address == 0) {
        return -1;
    }
    size_t max_sectors =
        static_cast<size_t>(sizeof(g_sync_block_io_buffer) / sector_size);
    if (max_sectors == 0) {
        return -1;
    }

    lock_sync_io();
    while (remaining > 0) {
        uint64_t chunk64 = remaining;
        if (chunk64 > 0xFFu) {
            chunk64 = 0xFFu;
        }
        if (chunk64 > max_sectors) {
            chunk64 = max_sectors;
        }
        uint8_t chunk = static_cast<uint8_t>(chunk64);
        fs::BlockIoStatus status =
            read_fn(record->device.context, static_cast<uint32_t>(current_lba),
                    chunk, g_sync_block_io_buffer);
        if (status != fs::BlockIoStatus::Ok) {
            unlock_sync_io();
            return -1;
        }
        size_t transfer_bytes =
            static_cast<size_t>(chunk) * static_cast<size_t>(sector_size);
        uint64_t dest = user_address +
                        (current_lba - lba) * sector_size;
        if (!copy_to_caller(proc, dest, g_sync_block_io_buffer, transfer_bytes)) {
            unlock_sync_io();
            return -1;
        }
        current_lba += chunk;
        remaining -= chunk;
    }
    unlock_sync_io();
    return static_cast<int64_t>(length);
}

int64_t block_device_write(process::Process& proc,
                           DescriptorEntry& entry,
                           uint64_t user_address,
                           uint64_t length,
                           uint64_t offset) {
    auto* record = static_cast<BlockDeviceRecord*>(entry.object);
    if (record == nullptr || !record->in_use) {
        return -1;
    }
    if (record->locked && !is_kernel_process(proc)) {
        return -1;
    }
    if (length == 0) {
        return 0;
    }
    uint64_t sector_size = record->device.sector_size;
    if (sector_size == 0) {
        return -1;
    }
    if ((offset % sector_size) != 0) {
        return -1;
    }
    if ((length % sector_size) != 0) {
        return -1;
    }
    uint64_t sector_count = length / sector_size;
    if (sector_count == 0) {
        return -1;
    }
    uint64_t lba = offset / sector_size;
    if (lba >= record->device.sector_count) {
        return -1;
    }
    if (lba + sector_count > record->device.sector_count) {
        return -1;
    }

    auto write_fn = record->device.write;
    if (write_fn == nullptr) {
        return -1;
    }
    uint64_t remaining = sector_count;
    uint64_t current_lba = lba;
    if (user_address == 0) {
        return -1;
    }
    size_t max_sectors =
        static_cast<size_t>(sizeof(g_sync_block_io_buffer) / sector_size);
    if (max_sectors == 0) {
        return -1;
    }

    lock_sync_io();
    while (remaining > 0) {
        uint64_t chunk64 = remaining;
        if (chunk64 > 0xFFu) {
            chunk64 = 0xFFu;
        }
        if (chunk64 > max_sectors) {
            chunk64 = max_sectors;
        }
        uint8_t chunk = static_cast<uint8_t>(chunk64);
        size_t transfer_bytes =
            static_cast<size_t>(chunk) * static_cast<size_t>(sector_size);
        uint64_t src = user_address +
                       (current_lba - lba) * sector_size;
        if (!copy_from_caller(proc, g_sync_block_io_buffer, src, transfer_bytes)) {
            unlock_sync_io();
            return -1;
        }
        fs::BlockIoStatus status =
            write_fn(record->device.context,
                     static_cast<uint32_t>(current_lba),
                     chunk,
                     g_sync_block_io_buffer);
        if (status != fs::BlockIoStatus::Ok) {
            unlock_sync_io();
            return -1;
        }
        current_lba += chunk;
        remaining -= chunk;
    }
    unlock_sync_io();
    return static_cast<int64_t>(length);
}

int block_device_get_property(DescriptorEntry& entry,
                              uint32_t property,
                              void* out,
                              size_t size) {
    auto* record = static_cast<BlockDeviceRecord*>(entry.object);
    if (record == nullptr || !record->in_use) {
        return -1;
    }
    if (property ==
        static_cast<uint32_t>(descriptor_defs::Property::BlockGeometry)) {
        if (out == nullptr || size < sizeof(descriptor_defs::BlockGeometry)) {
            return -1;
        }
        auto* geom = reinterpret_cast<descriptor_defs::BlockGeometry*>(out);
        geom->sector_size = record->device.sector_size;
        geom->sector_count = record->device.sector_count;
        return 0;
    }
    return -1;
}

const Ops kBlockDeviceOps{
    .read = block_device_read,
    .write = block_device_write,
    .get_property = block_device_get_property,
    .set_property = nullptr,
};

bool open_block_device(process::Process& proc,
                       uint64_t name_ptr,
                       uint64_t index,
                       uint64_t,
                       Allocation& alloc) {
    BlockDeviceRecord* record = nullptr;
    if (name_ptr != 0) {
        const char* name = reinterpret_cast<const char*>(name_ptr);
        record = find_block_device_by_name(name);
    } else {
        record = find_block_device_by_index(index);
    }
    if (record == nullptr || !record->in_use) {
        return false;
    }
    if (record->locked && !is_kernel_process(proc)) {
        return false;
    }
    alloc.type = kTypeBlockDevice;
    uint64_t flags = static_cast<uint64_t>(Flag::Seekable) |
                     static_cast<uint64_t>(Flag::Device) |
                     static_cast<uint64_t>(Flag::Block) |
                     static_cast<uint64_t>(Flag::Async);
    if (record->device.read != nullptr) {
        flags |= static_cast<uint64_t>(Flag::Readable);
    }
    if (record->device.write != nullptr) {
        flags |= static_cast<uint64_t>(Flag::Writable);
    }
    alloc.flags = flags;
    alloc.extended_flags = 0;
    alloc.has_extended_flags = false;
    alloc.object = record;
    alloc.subsystem_data = nullptr;
    alloc.close = nullptr;
    alloc.name = record->name;
    alloc.ops = &kBlockDeviceOps;
    return true;
}

void clear_block_devices() {
    for (auto& record : g_block_devices) {
        if (record.handle != kInvalidHandle) {
            close_kernel(record.handle);
        }
        record.in_use = false;
        record.name[0] = '\0';
        record.device = {};
        record.device.descriptor_handle = kInvalidHandle;
        record.handle = kInvalidHandle;
        record.locked = false;
    }
}

void service_requests(size_t sector_budget) {
    lock_requests();
    size_t processed = 0;
    for (auto& req : g_block_requests) {
        if (!req.in_use) {
            continue;
        }
        if (processed >= sector_budget) {
            break;
        }
        service_request(req, processed, sector_budget);
    }
    unlock_requests();
}

}  // namespace block_device_descriptor

bool register_block_device_descriptor() {
    return register_type(kTypeBlockDevice, block_device_descriptor::open_block_device, &block_device_descriptor::kBlockDeviceOps);
}

bool register_block_device(fs::BlockDevice& device, bool lock_for_kernel) {
    if (device.name == nullptr) {
        return false;
    }
    block_device_descriptor::BlockDeviceRecord* slot = block_device_descriptor::find_block_device_by_name(device.name);
    if (slot == nullptr) {
        for (auto& candidate : block_device_descriptor::g_block_devices) {
            if (!candidate.in_use) {
                slot = &candidate;
                break;
            }
        }
    }
    if (slot == nullptr) {
        log_message(LogLevel::Warn,
                    "Descriptor: block device registry full, dropping %s",
                    device.name);
        return false;
    }
    slot->device = device;
    string_util::copy(slot->name, block_device_descriptor::kMaxBlockDeviceNameLen, device.name);
    slot->device.name = slot->name;
    slot->locked = lock_for_kernel;
    slot->in_use = true;
    slot->handle = kInvalidHandle;
    slot->device.descriptor_handle = kInvalidHandle;
    device.name = slot->name;
    device.descriptor_handle = kInvalidHandle;

    if (lock_for_kernel) {
        slot->handle = open_kernel(kTypeBlockDevice,
                                   reinterpret_cast<uint64_t>(slot->name),
                                   0,
                                   0);
        if (slot->handle == kInvalidHandle) {
            log_message(LogLevel::Warn,
                        "Descriptor: failed to open block device descriptor for %s",
                        slot->name);
            slot->device = {};
            slot->in_use = false;
            slot->locked = false;
            slot->name[0] = '\0';
            device.descriptor_handle = kInvalidHandle;
            return false;
        }
        slot->device.descriptor_handle = slot->handle;
        device.descriptor_handle = slot->handle;
    }

    return true;
}

void reset_block_device_registry() {
    block_device_descriptor::clear_block_devices();
}

void service_block_io() {
    // Process a larger batch per tick to drain requests quickly while still
    // yielding to other work.
    constexpr size_t kSectorsPerTick = 64;
    block_device_descriptor::service_requests(kSectorsPerTick);
}

void block_io_worker(process::Process&) {
    // Run until the queue drains, then park this task until new work arrives.
    constexpr size_t kWorkerBudget = 256;
    block_device_descriptor::service_requests(kWorkerBudget);
    bool has_pending = false;
    for (auto& req : block_device_descriptor::g_block_requests) {
        if (req.in_use) {
            has_pending = true;
            break;
        }
    }
    if (!has_pending && block_device_descriptor::g_worker != nullptr) {
        block_device_descriptor::g_worker->state = process::State::Blocked;
        block_device_descriptor::g_worker->waiting_on = nullptr;
    }
}

void start_block_io_worker() {
    block_device_descriptor::lock_requests();
    if (block_device_descriptor::g_worker_started) {
        block_device_descriptor::unlock_requests();
        return;
    }
    block_device_descriptor::g_worker_started = true;
    block_device_descriptor::unlock_requests();
    process::Process* worker = process::allocate_kernel_task(block_io_worker);
    if (worker == nullptr) {
        log_message(LogLevel::Warn,
                    "BlockIO: failed to allocate kernel I/O worker");
        return;
    }
    block_device_descriptor::g_worker = worker;
    scheduler::enqueue(worker);
    log_message(LogLevel::Info, "BlockIO: kernel worker started (pid=%u)",
                static_cast<unsigned int>(worker->pid));
}

}  // namespace descriptor
