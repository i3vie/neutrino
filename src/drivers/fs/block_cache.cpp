#include "block_cache.hpp"

#include <stddef.h>
#include <stdint.h>

#include "drivers/log/logging.hpp"
#include "lib/mem.hpp"

namespace fs {
namespace block_cache {
namespace {

constexpr size_t kMaxCachedDevices = 32;
constexpr size_t kCacheEntryCount = 65536;
constexpr size_t kCacheSectorSize = 512;
constexpr size_t kIdleFlushBudget = 16;

struct CachedDevice {
    BlockDevice backing;
    bool in_use;
};

struct CacheEntry {
    CachedDevice* owner;
    uint32_t lba;
    uint64_t age;
    uint64_t generation;
    size_t sector_size;
    bool valid;
    bool dirty;
    bool flushing;
    alignas(512) uint8_t data[kCacheSectorSize];
};

CachedDevice g_devices[kMaxCachedDevices]{};
CacheEntry g_entries[kCacheEntryCount]{};
volatile int g_cache_lock = 0;
uint64_t g_clock = 0;
bool g_enabled = true;

void lock() {
    while (__atomic_test_and_set(&g_cache_lock, __ATOMIC_ACQUIRE)) {
        asm volatile("pause");
    }
}

void unlock() {
    __atomic_clear(&g_cache_lock, __ATOMIC_RELEASE);
}

class LockGuard {
public:
    LockGuard() { lock(); }
    ~LockGuard() { unlock(); }
};

void* byte_offset(void* ptr, size_t offset) {
    return static_cast<void*>(static_cast<uint8_t*>(ptr) + offset);
}

BlockIoStatus read_uncached(CachedDevice& cached,
                            uint32_t lba,
                            uint8_t sector_count,
                            void* buffer) {
    return block_read(cached.backing, lba, sector_count, buffer);
}

BlockIoStatus write_uncached(CachedDevice& cached,
                             uint32_t lba,
                             uint8_t sector_count,
                             const void* buffer) {
    return block_write(cached.backing, lba, sector_count, buffer);
}

CacheEntry* find_entry(CachedDevice& cached, uint32_t lba, size_t sector_size) {
    for (auto& entry : g_entries) {
        if (entry.valid && entry.owner == &cached && entry.lba == lba &&
            entry.sector_size == sector_size) {
            return &entry;
        }
    }
    return nullptr;
}

CacheEntry* choose_clean_victim() {
    CacheEntry* oldest = nullptr;
    for (auto& entry : g_entries) {
        if (!entry.valid) {
            return &entry;
        }
        if (entry.dirty || entry.flushing) {
            continue;
        }
        if (oldest == nullptr) {
            oldest = &entry;
            continue;
        }
        if (entry.age < oldest->age) {
            oldest = &entry;
        }
    }
    return oldest;
}

bool flush_one_dirty() {
    CachedDevice* owner = nullptr;
    uint32_t lba = 0;
    uint64_t generation = 0;
    size_t sector_size = 0;
    alignas(512) uint8_t sector_buffer[kCacheSectorSize];

    lock();
    CacheEntry* entry = nullptr;
    for (auto& candidate : g_entries) {
        if (!candidate.valid || !candidate.dirty || candidate.flushing) {
            continue;
        }
        if (entry == nullptr || candidate.age < entry->age) {
            entry = &candidate;
        }
    }
    if (entry == nullptr) {
        unlock();
        return false;
    }

    owner = entry->owner;
    lba = entry->lba;
    generation = entry->generation;
    sector_size = entry->sector_size;
    entry->flushing = true;
    memcpy(sector_buffer, entry->data, sector_size);
    unlock();

    BlockIoStatus status = write_uncached(*owner, lba, 1, sector_buffer);

    lock();
    for (auto& candidate : g_entries) {
        if (!candidate.valid || candidate.owner != owner ||
            candidate.lba != lba || candidate.generation != generation) {
            continue;
        }
        candidate.flushing = false;
        if (status == BlockIoStatus::Ok) {
            candidate.dirty = false;
        }
        break;
    }
    unlock();
    return true;
}

CacheEntry* find_or_prepare_entry_locked(CachedDevice& cached,
                                         uint32_t lba,
                                         size_t sector_size) {
    CacheEntry* entry = find_entry(cached, lba, sector_size);
    if (entry != nullptr) {
        return entry;
    }
    entry = choose_clean_victim();
    if (entry == nullptr) {
        return nullptr;
    }
    entry->owner = &cached;
    entry->lba = lba;
    entry->sector_size = sector_size;
    entry->valid = true;
    entry->dirty = false;
    entry->flushing = false;
    return entry;
}

BlockIoStatus cached_read(void* context,
                          uint32_t lba,
                          uint8_t sector_count,
                          void* buffer) {
    auto* cached = static_cast<CachedDevice*>(context);
    if (cached == nullptr || buffer == nullptr) {
        return BlockIoStatus::IoError;
    }
    if (sector_count == 0) {
        return BlockIoStatus::Ok;
    }
    if (cached->backing.sector_size == 0 ||
        cached->backing.sector_size != kCacheSectorSize ||
        !g_enabled) {
        return read_uncached(*cached, lba, sector_count, buffer);
    }

    const size_t sector_size = cached->backing.sector_size;
    for (uint8_t i = 0; i < sector_count; ++i) {
        uint32_t sector_lba = lba + i;
        void* out = byte_offset(buffer, static_cast<size_t>(i) * sector_size);

        lock();
        CacheEntry* entry = find_entry(*cached, sector_lba, sector_size);
        if (entry != nullptr) {
            ++g_clock;
            entry->age = g_clock;
            memcpy(out, entry->data, sector_size);
            unlock();
            continue;
        }
        unlock();

        alignas(512) uint8_t sector_buffer[kCacheSectorSize];
        BlockIoStatus status =
            read_uncached(*cached, sector_lba, 1, sector_buffer);
        if (status != BlockIoStatus::Ok) {
            return status;
        }

        for (;;) {
            lock();
            entry = find_or_prepare_entry_locked(*cached,
                                                 sector_lba,
                                                 sector_size);
            if (entry != nullptr) {
                memcpy(entry->data, sector_buffer, sector_size);
                ++g_clock;
                ++entry->generation;
                entry->age = g_clock;
                unlock();
                break;
            }
            unlock();
            if (!flush_one_dirty()) {
                break;
            }
        }
        memcpy(out, sector_buffer, sector_size);
    }
    return BlockIoStatus::Ok;
}

BlockIoStatus cached_write(void* context,
                           uint32_t lba,
                           uint8_t sector_count,
                           const void* buffer) {
    auto* cached = static_cast<CachedDevice*>(context);
    if (cached == nullptr || buffer == nullptr) {
        return BlockIoStatus::IoError;
    }
    if (sector_count == 0) {
        return BlockIoStatus::Ok;
    }

    if (cached->backing.sector_size == 0 ||
        cached->backing.sector_size != kCacheSectorSize ||
        !g_enabled) {
        return write_uncached(*cached, lba, sector_count, buffer);
    }

    const size_t sector_size = cached->backing.sector_size;
    uint8_t cached_count = 0;
    while (cached_count < sector_count) {
        bool cache_full = false;
        lock();
        while (cached_count < sector_count) {
            uint32_t sector_lba = lba + cached_count;
            const void* in = static_cast<const void*>(
                static_cast<const uint8_t*>(buffer) +
                static_cast<size_t>(cached_count) * sector_size);
            CacheEntry* entry = find_or_prepare_entry_locked(*cached,
                                                             sector_lba,
                                                             sector_size);
            if (entry == nullptr) {
                cache_full = true;
                break;
            }
            memcpy(entry->data, in, sector_size);
            ++g_clock;
            ++entry->generation;
            entry->age = g_clock;
            entry->dirty = true;
            entry->flushing = false;
            ++cached_count;
        }
        unlock();

        if (cache_full && !flush_one_dirty()) {
            const void* in = static_cast<const void*>(
                static_cast<const uint8_t*>(buffer) +
                static_cast<size_t>(cached_count) * sector_size);
            BlockIoStatus status =
                write_uncached(*cached, lba + cached_count, 1, in);
            if (status != BlockIoStatus::Ok) {
                return status;
            }
            ++cached_count;
        }
    }
    return BlockIoStatus::Ok;
}

}  // namespace

void init() {
    LockGuard guard;
    for (auto& device : g_devices) {
        device = {};
    }
    for (auto& entry : g_entries) {
        entry = {};
    }
    g_clock = 0;
    g_enabled = true;
}

void service_idle_flush() {
    for (size_t i = 0; i < kIdleFlushBudget; ++i) {
        if (!flush_one_dirty()) {
            break;
        }
    }
}

void set_enabled(bool enabled) {
    if (!enabled) {
        while (flush_one_dirty()) {
        }
    }
    LockGuard guard;
    g_enabled = enabled;
}

bool enabled() {
    return g_enabled;
}

bool wrap_device(const BlockDevice& backing, BlockDevice& out_device) {
    out_device = backing;
    if (backing.sector_size == 0 || backing.sector_size != kCacheSectorSize ||
        backing.read == cached_read || backing.write == cached_write) {
        return true;
    }

    LockGuard guard;
    for (auto& device : g_devices) {
        if (device.in_use) {
            continue;
        }
        device.backing = backing;
        device.in_use = true;

        out_device.read = cached_read;
        out_device.write = cached_write;
        out_device.context = &device;
        out_device.descriptor_handle = descriptor::kInvalidHandle;
        return true;
    }

    log_message(LogLevel::Warn,
                "BlockCache: no wrapper slots for %s",
                backing.name != nullptr ? backing.name : "(unnamed)");
    return false;
}

void invalidate_device(const BlockDevice& device) {
    if (device.read != cached_read && device.write != cached_write) {
        return;
    }
    auto* cached = static_cast<CachedDevice*>(device.context);

    for (;;) {
        bool has_dirty = false;
        lock();
        for (auto& entry : g_entries) {
            if (entry.valid && entry.owner == cached && entry.dirty) {
                has_dirty = true;
                break;
            }
        }
        unlock();
        if (!has_dirty || !flush_one_dirty()) {
            break;
        }
    }

    LockGuard guard;
    for (auto& entry : g_entries) {
        if (entry.owner == cached) {
            entry.valid = false;
            entry.dirty = false;
            entry.flushing = false;
        }
    }
}

}  // namespace block_cache
}  // namespace fs
