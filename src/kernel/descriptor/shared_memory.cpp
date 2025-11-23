#include "../descriptor.hpp"

#include "../../lib/mem.hpp"
#include "../string_util.hpp"
#include "../vm.hpp"

namespace descriptor {

namespace shared_memory_descriptor {

constexpr size_t kMaxSegments = 32;
constexpr size_t kMaxNameLength = 48;
constexpr size_t kDefaultSegmentSize = 0x1000;

struct SharedSegment {
    bool in_use;
    char name[kMaxNameLength];
    vm::Region region;
    uint32_t refcount;
};

SharedSegment g_segments[kMaxSegments];
volatile int g_segments_lock = 0;

void lock_segments() {
    while (__atomic_test_and_set(&g_segments_lock, __ATOMIC_ACQUIRE)) {
        asm volatile("pause");
    }
}

void unlock_segments() {
    __atomic_clear(&g_segments_lock, __ATOMIC_RELEASE);
}

class SegmentGuard {
public:
    SegmentGuard() { lock_segments(); }
    ~SegmentGuard() { unlock_segments(); }
};

void reset_segment(SharedSegment& segment) {
    segment.in_use = false;
    segment.name[0] = '\0';
    segment.region = vm::Region{0, 0};
    segment.refcount = 0;
}

SharedSegment* find_segment_locked(const char* name) {
    if (name == nullptr || name[0] == '\0') {
        return nullptr;
    }
    for (auto& segment : g_segments) {
        if (!segment.in_use) {
            continue;
        }
        if (string_util::equals(segment.name, name)) {
            return &segment;
        }
    }
    return nullptr;
}

SharedSegment* allocate_segment_locked(const char* name,
                                       size_t requested_length) {
    size_t length = (requested_length == 0) ? kDefaultSegmentSize
                                            : requested_length;
    vm::Region region = vm::allocate_user_region(length);
    if (region.base == 0 || region.length == 0) {
        return nullptr;
    }
    if (!vm::is_user_range(region.base, region.length)) {
        vm::release_user_region(region);
        return nullptr;
    }
    for (auto& segment : g_segments) {
        if (segment.in_use) {
            continue;
        }
        reset_segment(segment);
        segment.in_use = true;
        segment.region = region;
        string_util::copy(segment.name, sizeof(segment.name), name);
        return &segment;
    }
    vm::release_user_region(region);
    return nullptr;
}

int64_t shared_memory_read(process::Process&,
                           DescriptorEntry& entry,
                           uint64_t user_address,
                           uint64_t length,
                           uint64_t offset) {
    auto* segment = static_cast<SharedSegment*>(entry.object);
    if (segment == nullptr || !segment->in_use) {
        return -1;
    }
    if (segment->region.length == 0 || user_address == 0) {
        return -1;
    }
    if (offset >= segment->region.length) {
        return 0;
    }
    uint64_t remaining = segment->region.length - offset;
    uint64_t to_copy = (length > remaining) ? remaining : length;
    if (to_copy == 0) {
        return 0;
    }
    if (!vm::is_user_range(user_address, to_copy)) {
        return -1;
    }
    auto* dest = reinterpret_cast<uint8_t*>(user_address);
    auto* src =
        reinterpret_cast<const uint8_t*>(segment->region.base + offset);
    memcpy(dest, src, static_cast<size_t>(to_copy));
    return static_cast<int64_t>(to_copy);
}

int64_t shared_memory_write(process::Process&,
                            DescriptorEntry& entry,
                            uint64_t user_address,
                            uint64_t length,
                            uint64_t offset) {
    auto* segment = static_cast<SharedSegment*>(entry.object);
    if (segment == nullptr || !segment->in_use) {
        return -1;
    }
    if (segment->region.length == 0 || user_address == 0) {
        return -1;
    }
    if (offset >= segment->region.length) {
        return 0;
    }
    uint64_t remaining = segment->region.length - offset;
    uint64_t to_copy = (length > remaining) ? remaining : length;
    if (to_copy == 0) {
        return 0;
    }
    if (!vm::is_user_range(user_address, to_copy)) {
        return -1;
    }
    const auto* src = reinterpret_cast<const uint8_t*>(user_address);
    auto* dest =
        reinterpret_cast<uint8_t*>(segment->region.base + offset);
    memcpy(dest, src, static_cast<size_t>(to_copy));
    return static_cast<int64_t>(to_copy);
}

int shared_memory_get_property(DescriptorEntry& entry,
                               uint32_t property,
                               void* out,
                               size_t size) {
    auto* segment = static_cast<SharedSegment*>(entry.object);
    if (segment == nullptr || !segment->in_use) {
        return -1;
    }
    if (property ==
        static_cast<uint32_t>(descriptor_defs::Property::SharedMemoryInfo)) {
        if (out == nullptr || size < sizeof(descriptor_defs::SharedMemoryInfo)) {
            return -1;
        }
        if (!vm::is_user_range(reinterpret_cast<uint64_t>(out),
                               sizeof(descriptor_defs::SharedMemoryInfo))) {
            return -1;
        }
        auto* info =
            reinterpret_cast<descriptor_defs::SharedMemoryInfo*>(out);
        info->base = segment->region.base;
        info->length = segment->region.length;
        return 0;
    }
    return -1;
}

void shared_memory_close(DescriptorEntry& entry) {
    auto* segment = static_cast<SharedSegment*>(entry.object);
    if (segment == nullptr) {
        return;
    }
    SegmentGuard guard;
    if (segment->refcount > 0) {
        --segment->refcount;
    }
    if (segment->refcount == 0 && segment->in_use) {
        vm::release_user_region(segment->region);
        reset_segment(*segment);
    }
}

const Ops kSharedMemoryOps{
    .read = shared_memory_read,
    .write = shared_memory_write,
    .get_property = shared_memory_get_property,
    .set_property = nullptr,
};

bool open_shared_memory(process::Process&,
                        uint64_t name_ptr,
                        uint64_t length,
                        uint64_t,
                        Allocation& alloc) {
    if (name_ptr == 0) {
        return false;
    }
    const char* user_name = reinterpret_cast<const char*>(name_ptr);
    char name_buffer[kMaxNameLength];
    if (!vm::copy_user_string(user_name, name_buffer, sizeof(name_buffer))) {
        return false;
    }
    if (name_buffer[0] == '\0') {
        return false;
    }
    size_t requested = static_cast<size_t>(length);
    SharedSegment* segment = nullptr;
    {
        SegmentGuard guard;
        segment = find_segment_locked(name_buffer);
        if (segment == nullptr) {
            segment = allocate_segment_locked(name_buffer, requested);
            if (segment == nullptr) {
                return false;
            }
        } else if (requested != 0 && segment->region.length < requested) {
            return false;
        }
        ++segment->refcount;
    }
    alloc.type = kTypeSharedMemory;
    alloc.flags = static_cast<uint64_t>(Flag::Readable) |
                  static_cast<uint64_t>(Flag::Writable) |
                  static_cast<uint64_t>(Flag::Mappable);
    alloc.extended_flags = 0;
    alloc.has_extended_flags = false;
    alloc.object = segment;
    alloc.subsystem_data = nullptr;
    alloc.name = segment->name;
    alloc.ops = &kSharedMemoryOps;
    alloc.close = shared_memory_close;
    return true;
}

}  // namespace shared_memory_descriptor

bool register_shared_memory_descriptor() {
    return register_type(kTypeSharedMemory,
                         shared_memory_descriptor::open_shared_memory,
                         &shared_memory_descriptor::kSharedMemoryOps);
}

}  // namespace descriptor
