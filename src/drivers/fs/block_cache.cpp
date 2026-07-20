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
constexpr size_t kCacheHashBucketCount = 65536;
constexpr size_t kCacheSectorSize = 512;
constexpr size_t kIdleFlushBudget = 16;
constexpr uint16_t kSequentialWriteThroughSectors = 4;

struct CachedDevice {
    BlockDevice backing;
    bool in_use;
    bool have_last_write;
    uint16_t sequential_write_sectors;
    uint32_t last_write_end_lba;
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
    int32_t hash_next;
    alignas(512) uint8_t data[kCacheSectorSize];
};

CachedDevice g_devices[kMaxCachedDevices]{};
CacheEntry g_entries[kCacheEntryCount]{};
int32_t g_hash_heads[kCacheHashBucketCount]{};
volatile int g_cache_lock = 0;
uint64_t g_clock = 0;
size_t g_victim_cursor = 0;
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

size_t cache_hash(const CachedDevice& cached, uint32_t lba) {
    uintptr_t owner = reinterpret_cast<uintptr_t>(&cached);
    uint64_t mixed = static_cast<uint64_t>(lba) * 11400714819323198485ull;
    mixed ^= static_cast<uint64_t>(owner >> 4);
    return static_cast<size_t>(mixed) & (kCacheHashBucketCount - 1);
}

CacheEntry* find_entry(CachedDevice& cached, uint32_t lba, size_t sector_size) {
    int32_t index = g_hash_heads[cache_hash(cached, lba)];
    while (index >= 0) {
        CacheEntry& entry = g_entries[static_cast<size_t>(index)];
        if (entry.valid && entry.owner == &cached && entry.lba == lba &&
            entry.sector_size == sector_size) {
            return &entry;
        }
        index = entry.hash_next;
    }
    return nullptr;
}

void unlink_entry_locked(CacheEntry& target) {
    if (!target.valid || target.owner == nullptr) {
        target.hash_next = -1;
        return;
    }
    size_t bucket = cache_hash(*target.owner, target.lba);
    int32_t* link = &g_hash_heads[bucket];
    while (*link >= 0) {
        CacheEntry& entry = g_entries[static_cast<size_t>(*link)];
        if (&entry == &target) {
            *link = entry.hash_next;
            entry.hash_next = -1;
            return;
        }
        link = &entry.hash_next;
    }
    target.hash_next = -1;
}

void link_entry_locked(CacheEntry& entry) {
    size_t index = static_cast<size_t>(&entry - g_entries);
    size_t bucket = cache_hash(*entry.owner, entry.lba);
    entry.hash_next = g_hash_heads[bucket];
    g_hash_heads[bucket] = static_cast<int32_t>(index);
}

CacheEntry* choose_clean_victim() {
    CacheEntry* oldest = nullptr;
    for (size_t scanned = 0; scanned < kCacheEntryCount; ++scanned) {
        size_t index = (g_victim_cursor + scanned) % kCacheEntryCount;
        CacheEntry& entry = g_entries[index];
        if (!entry.valid) {
            g_victim_cursor = (index + 1) % kCacheEntryCount;
            return &entry;
        }
    }
    for (auto& entry : g_entries) {
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

bool flush_dirty_entry(CacheEntry* target) {
    CachedDevice* owner = nullptr;
    uint32_t lba = 0;
    uint64_t generation = 0;
    size_t sector_size = 0;
    alignas(512) uint8_t sector_buffer[kCacheSectorSize];

    lock();
    if (target == nullptr || !target->valid || !target->dirty ||
        target->flushing) {
        unlock();
        return true;
    }

    owner = target->owner;
    lba = target->lba;
    generation = target->generation;
    sector_size = target->sector_size;
    target->flushing = true;
    memcpy(sector_buffer, target->data, sector_size);
    unlock();

    BlockIoStatus status = write_uncached(*owner, lba, 1, sector_buffer);

    lock();
    if (target->valid && target->owner == owner && target->lba == lba &&
        target->generation == generation) {
        target->flushing = false;
        if (status == BlockIoStatus::Ok) {
            target->dirty = false;
        }
    }
    unlock();
    return status == BlockIoStatus::Ok;
}

bool flush_one_dirty() {
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
    unlock();

    if (entry == nullptr) {
        return false;
    }
    return flush_dirty_entry(entry);
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
    if (entry->valid) {
        unlink_entry_locked(*entry);
    }
    entry->owner = &cached;
    entry->lba = lba;
    entry->sector_size = sector_size;
    entry->valid = true;
    entry->dirty = false;
    entry->flushing = false;
    link_entry_locked(*entry);
    return entry;
}

void invalidate_write_range(CachedDevice& cached,
                            uint32_t lba,
                            uint8_t sector_count) {
    LockGuard guard;
    for (uint16_t i = 0; i < sector_count; ++i) {
        CacheEntry* entry = find_entry(cached,
                                       lba + static_cast<uint32_t>(i),
                                       cached.backing.sector_size);
        if (entry == nullptr) {
            continue;
        }
        unlink_entry_locked(*entry);
        entry->owner = nullptr;
        entry->valid = false;
        entry->dirty = false;
        entry->flushing = false;
    }
}

bool should_write_through(CachedDevice& cached,
                          uint32_t lba,
                          uint8_t sector_count) {
    LockGuard guard;
    if (cached.have_last_write && lba == cached.last_write_end_lba) {
        uint32_t total = static_cast<uint32_t>(cached.sequential_write_sectors) +
                         static_cast<uint32_t>(sector_count);
        cached.sequential_write_sectors =
            static_cast<uint16_t>(total > UINT16_MAX ? UINT16_MAX : total);
    } else {
        cached.sequential_write_sectors = sector_count;
    }
    cached.have_last_write = true;
    cached.last_write_end_lba = lba + static_cast<uint32_t>(sector_count);
    return sector_count > 1 ||
           cached.sequential_write_sectors >= kSequentialWriteThroughSectors;
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

    // Full-cluster and sustained sequential writes are streaming I/O, even on
    // FAT volumes that use one sector per cluster. Caching them only turns the
    // transfer into a 32 MiB burst followed by one-sector writeback once the
    // cache is full.
    if (should_write_through(*cached, lba, sector_count)) {
        invalidate_write_range(*cached, lba, sector_count);
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
        entry.hash_next = -1;
    }
    for (auto& head : g_hash_heads) {
        head = -1;
    }
    g_clock = 0;
    g_victim_cursor = 0;
    g_enabled = true;
}

void service_idle_flush() {
    for (size_t i = 0; i < kIdleFlushBudget; ++i) {
        if (!flush_one_dirty()) {
            break;
        }
    }
}

bool flush_all() {
    for (;;) {
        bool has_dirty = false;
        bool flushed_any = false;
        bool failed_any = false;

        for (auto& entry : g_entries) {
            lock();
            bool dirty = entry.valid && entry.dirty && !entry.flushing;
            unlock();
            if (!dirty) {
                continue;
            }

            has_dirty = true;
            if (flush_dirty_entry(&entry)) {
                flushed_any = true;
            } else {
                failed_any = true;
            }
        }

        if (!has_dirty) {
            return true;
        }
        if (failed_any && !flushed_any) {
            return false;
        }
    }
}

void set_enabled(bool enabled) {
    if (!enabled) {
        (void)flush_all();
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
        out_device.write = backing.write != nullptr ? cached_write : nullptr;
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
            unlink_entry_locked(entry);
            entry.owner = nullptr;
            entry.valid = false;
            entry.dirty = false;
            entry.flushing = false;
        }
    }
}

}  // namespace block_cache
}  // namespace fs
