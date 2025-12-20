#include "../descriptor.hpp"

#include "../../arch/x86_64/memory/paging.hpp"
#include "../../drivers/log/logging.hpp"
#include "../../lib/mem.hpp"
#include "../process.hpp"
#include "../string_util.hpp"
#include "../vm.hpp"

namespace descriptor {

namespace shared_memory_descriptor {

constexpr size_t kMaxSegments = 32;
constexpr size_t kMaxNameLength = 48;
constexpr size_t kDefaultSegmentSize = 0x1000;
constexpr uint64_t kPageSize = 0x1000;
constexpr uint64_t kPageMask = kPageSize - 1;
constexpr size_t kEntriesPerChunk =
    (kPageSize - sizeof(void*) - sizeof(size_t)) / sizeof(uint64_t);

struct PhysPageChunk {
    PhysPageChunk* next;
    size_t count;
    uint64_t entries[kEntriesPerChunk];
};

constexpr uint64_t align_down(uint64_t value, uint64_t alignment) {
    return value & ~(alignment - 1);
}

constexpr uint64_t align_up(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

struct SharedSegment {
    bool in_use;
    char name[kMaxNameLength];
    vm::Region region;
    uint32_t refcount;
    PhysPageChunk* pages;
    uint32_t owner_pid;
    uint64_t owner_cr3;
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
    segment.pages = nullptr;
    segment.owner_pid = 0;
    segment.owner_cr3 = 0;
}

PhysPageChunk* allocate_chunk() {
    auto* chunk = static_cast<PhysPageChunk*>(paging_alloc_page());
    if (chunk != nullptr) {
        memset(chunk, 0, sizeof(PhysPageChunk));
    }
    return chunk;
}

void free_chunk_list(PhysPageChunk* head) {
    PhysPageChunk* current = head;
    while (current != nullptr) {
        PhysPageChunk* next = current->next;
        uint64_t phys =
            paging_virt_to_phys(reinterpret_cast<uint64_t>(current));
        paging_free_physical(phys);
        current = next;
    }
}

bool append_phys_page(PhysPageChunk*& head, PhysPageChunk*& tail,
                      uint64_t phys) {
    if (tail == nullptr || tail->count >= kEntriesPerChunk) {
        PhysPageChunk* chunk = allocate_chunk();
        if (chunk == nullptr) {
            return false;
        }
        if (tail != nullptr) {
            tail->next = chunk;
        }
        tail = chunk;
        if (head == nullptr) {
            head = chunk;
        }
    }
    tail->entries[tail->count++] = phys;
    return true;
}

void destroy_segment_metadata(SharedSegment& segment) {
    free_chunk_list(segment.pages);
    segment.pages = nullptr;
}

bool record_segment_pages(process::Process& proc, SharedSegment& segment) {
    uint64_t base = align_down(segment.region.base, kPageSize);
    size_t pages = static_cast<size_t>(
        align_up(segment.region.length, kPageSize) / kPageSize);
    PhysPageChunk* head = nullptr;
    PhysPageChunk* tail = nullptr;
    for (size_t i = 0; i < pages; ++i) {
        uint64_t virt = base + static_cast<uint64_t>(i) * kPageSize;
        uint64_t phys = 0;
        if (!paging_translate(proc.cr3, virt, phys)) {
            destroy_segment_metadata(segment);
            return false;
        }
        uint64_t phys_page = phys & ~kPageMask;
        if (!append_phys_page(head, tail, phys_page)) {
            destroy_segment_metadata(segment);
            return false;
        }
    }
    segment.pages = head;
    return true;
}

bool for_each_segment_page(const SharedSegment& segment,
                           bool (*callback)(size_t, uint64_t, void*),
                           void* ctx) {
    size_t index = 0;
    for (PhysPageChunk* chunk = segment.pages; chunk != nullptr;
         chunk = chunk->next) {
        for (size_t i = 0; i < chunk->count; ++i) {
            if (!callback(index, chunk->entries[i], ctx)) {
                return false;
            }
            ++index;
        }
    }
    return true;
}

bool locate_segment_page(const SharedSegment& segment,
                         size_t page_index,
                         uint64_t& phys_out) {
    size_t index = 0;
    for (PhysPageChunk* chunk = segment.pages; chunk != nullptr;
         chunk = chunk->next) {
        if (page_index < index + chunk->count) {
            size_t local = page_index - index;
            phys_out = chunk->entries[local];
            return true;
        }
        index += chunk->count;
    }
    phys_out = 0;
    return false;
}

struct MapContext {
    process::Process* proc;
    const SharedSegment* segment;
    uint64_t base;
};

bool map_page_callback(size_t index, uint64_t phys, void* ctx) {
    auto* context = static_cast<MapContext*>(ctx);
    if (context == nullptr || context->proc == nullptr ||
        context->segment == nullptr) {
        return false;
    }
    uint64_t virt = context->base +
                    static_cast<uint64_t>(index) * kPageSize;
    uint64_t existing = 0;
    if (paging_translate(context->proc->cr3, virt, existing)) {
        if ((existing & ~kPageMask) == phys) {
            return true;
        }
        log_message(LogLevel::Warn,
                    "SharedMemory: remapping pid=%u virt=%llx existing=%llx new=%llx",
                    static_cast<unsigned int>(context->proc->pid),
                    static_cast<unsigned long long>(virt),
                    static_cast<unsigned long long>(existing & ~kPageMask),
                    static_cast<unsigned long long>(phys));
        uint64_t dummy = 0;
        paging_unmap_page_in_space(context->proc->cr3, virt, dummy);
        if (process::current() == context->proc) {
            asm volatile("invlpg (%0)" : : "r"(reinterpret_cast<void*>(virt)) : "memory");
        }
    }
    return paging_map_page_in_space(context->proc->cr3,
                                    virt,
                                    phys,
                                    PAGE_FLAG_WRITE | PAGE_FLAG_USER);
}

struct UnmapContext {
    process::Process* proc;
    const SharedSegment* segment;
    uint64_t base;
};

bool unmap_page_callback(size_t index, uint64_t phys, void* ctx) {
    auto* context = static_cast<UnmapContext*>(ctx);
    if (context == nullptr || context->proc == nullptr ||
        context->segment == nullptr) {
        return false;
    }
    uint64_t virt = context->base +
                    static_cast<uint64_t>(index) * kPageSize;
    uint64_t existing = 0;
    if (!paging_translate(context->proc->cr3, virt, existing)) {
        return true;
    }
    if ((existing & ~kPageMask) != phys) {
        return true;
    }
    uint64_t dummy = 0;
    paging_unmap_page_in_space(context->proc->cr3, virt, dummy);
    return true;
}

bool free_page_callback(size_t, uint64_t phys, void*) {
    paging_free_physical(phys);
    return true;
}

bool map_segment_into_process(SharedSegment& segment,
                              process::Process& proc,
                              uint64_t base) {
    MapContext context{&proc, &segment, base};
    bool ok = for_each_segment_page(segment, map_page_callback, &context);
    if (!ok) {
        log_message(LogLevel::Error,
                    "SharedMemory: failed to map segment '%s' into pid=%u",
                    segment.name,
                    static_cast<unsigned int>(proc.pid));
    }
    return ok;
}

bool ensure_segment_mapping(process::Process& proc,
                            SharedSegment& segment,
                            uint64_t base) {
    if (proc.cr3 == 0) {
        return true;
    }
    return map_segment_into_process(segment, proc, base);
}

void unmap_segment_from_process(process::Process& proc,
                                const SharedSegment& segment,
                                uint64_t base) {
    UnmapContext context{&proc, &segment, base};
    for_each_segment_page(segment, unmap_page_callback, &context);
}

void unmap_segment_from_all_processes(const SharedSegment& segment) {
    for (size_t i = 0; i < process::kMaxProcesses; ++i) {
        process::Process* proc = process::table_entry(i);
        if (proc == nullptr || proc->cr3 == 0) {
            continue;
        }
        unmap_segment_from_process(*proc, segment, segment.region.base);
    }
}

void release_segment_pages(SharedSegment& segment) {
    for_each_segment_page(segment, free_page_callback, nullptr);
    destroy_segment_metadata(segment);
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
                                       size_t requested_length,
                                       process::Process& owner) {
    size_t length = (requested_length == 0) ? kDefaultSegmentSize
                                            : requested_length;
    vm::Region region = vm::allocate_user_shared_region(owner.cr3, length);
    if (region.base == 0 || region.length == 0) {
        return nullptr;
    }
    if (!vm::is_user_range(region.base, region.length)) {
        vm::release_user_region(owner.cr3, region);
        return nullptr;
    }
    for (auto& segment : g_segments) {
        if (segment.in_use) {
            continue;
        }
        reset_segment(segment);
        segment.in_use = true;
        segment.region = region;
        segment.owner_pid = owner.pid;
        segment.owner_cr3 = owner.cr3;
        if (!record_segment_pages(owner, segment)) {
            segment.in_use = false;
            vm::release_user_region(owner.cr3, region);
            return nullptr;
        }
        string_util::copy(segment.name, sizeof(segment.name), name);
        return &segment;
    }
    vm::release_user_region(owner.cr3, region);
    return nullptr;
}

int64_t shared_memory_read(process::Process& proc,
                           DescriptorEntry& entry,
                           uint64_t user_address,
                           uint64_t length,
                           uint64_t offset) {
    auto* segment = static_cast<SharedSegment*>(entry.object);
    if (segment == nullptr || !segment->in_use) {
        return -1;
    }
    if (user_address == 0) {
        return -1;
    }
    uint64_t mapping_base = segment->region.base;
    uint64_t mapping_length = segment->region.length;
    if (mapping_length == 0 || offset >= mapping_length) {
        return 0;
    }
    uint64_t remaining = mapping_length - offset;
    uint64_t to_copy = (length > remaining) ? remaining : length;
    if (to_copy == 0) {
        return 0;
    }
    bool kernel_process = (proc.cr3 == 0);
    if (!kernel_process) {
        if (!vm::is_user_range(user_address, to_copy)) {
            return -1;
        }
        if (!ensure_segment_mapping(proc, *segment, mapping_base)) {
            return -1;
        }
    }
    uint64_t copied = 0;
    while (copied < to_copy) {
        uint64_t current = offset + copied;
        size_t page_index = static_cast<size_t>(current / kPageSize);
        size_t page_offset = static_cast<size_t>(current & kPageMask);
        uint64_t phys_page = 0;
        if (!locate_segment_page(*segment, page_index, phys_page)) {
            return -1;
        }
        size_t chunk = static_cast<size_t>(to_copy - copied);
        size_t available = static_cast<size_t>(kPageSize - page_offset);
        if (chunk > available) {
            chunk = available;
        }
        uint64_t shared_ptr = paging_phys_to_virt(phys_page) + page_offset;
        const void* src = reinterpret_cast<const void*>(shared_ptr);
        uint64_t dest_addr = user_address + copied;
        if (kernel_process) {
            auto* dest_ptr = reinterpret_cast<uint8_t*>(dest_addr);
            memcpy(dest_ptr, src, chunk);
        } else {
            if (!vm::copy_into_address_space(proc.cr3,
                                             dest_addr,
                                             src,
                                             chunk)) {
                return -1;
            }
        }
        copied += chunk;
    }
    return static_cast<int64_t>(copied);
}

int64_t shared_memory_write(process::Process& proc,
                            DescriptorEntry& entry,
                            uint64_t user_address,
                            uint64_t length,
                            uint64_t offset) {
    auto* segment = static_cast<SharedSegment*>(entry.object);
    if (segment == nullptr || !segment->in_use) {
        return -1;
    }
    if (user_address == 0) {
        return -1;
    }
    uint64_t mapping_base = segment->region.base;
    uint64_t mapping_length = segment->region.length;
    if (mapping_length == 0 || offset >= mapping_length) {
        return 0;
    }
    uint64_t remaining = mapping_length - offset;
    uint64_t to_copy = (length > remaining) ? remaining : length;
    if (to_copy == 0) {
        return 0;
    }
    bool kernel_process = (proc.cr3 == 0);
    if (!kernel_process) {
        if (!vm::is_user_range(user_address, to_copy)) {
            return -1;
        }
        if (!ensure_segment_mapping(proc, *segment, mapping_base)) {
            return -1;
        }
    }
    uint64_t copied = 0;
    while (copied < to_copy) {
        uint64_t current = offset + copied;
        size_t page_index = static_cast<size_t>(current / kPageSize);
        size_t page_offset = static_cast<size_t>(current & kPageMask);
        uint64_t phys_page = 0;
        if (!locate_segment_page(*segment, page_index, phys_page)) {
            return -1;
        }
        size_t chunk = static_cast<size_t>(to_copy - copied);
        size_t available = static_cast<size_t>(kPageSize - page_offset);
        if (chunk > available) {
            chunk = available;
        }
        uint64_t shared_ptr = paging_phys_to_virt(phys_page) + page_offset;
        void* dest = reinterpret_cast<void*>(shared_ptr);
        uint64_t src_addr = user_address + copied;
        if (kernel_process) {
            const auto* src_ptr = reinterpret_cast<const uint8_t*>(src_addr);
            memcpy(dest, src_ptr, chunk);
        } else {
            if (!vm::copy_from_address_space(proc.cr3,
                                             dest,
                                             src_addr,
                                             chunk)) {
                return -1;
            }
        }
        copied += chunk;
    }
    return static_cast<int64_t>(copied);
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
    process::Process* proc = process::current();
    if (segment != nullptr && proc != nullptr) {
        unmap_segment_from_process(*proc, *segment, segment->region.base);
    }
    if (segment == nullptr) {
        return;
    }
    SegmentGuard guard;
    if (segment->refcount > 0) {
        --segment->refcount;
    }
    if (segment->refcount == 0 && segment->in_use) {
        release_segment_pages(*segment);
        if (segment->owner_pid != 0 && segment->owner_cr3 != 0) {
            vm::release_user_region(segment->owner_cr3, segment->region);
        }
        reset_segment(*segment);
    }
}

const Ops kSharedMemoryOps{
    .read = shared_memory_read,
    .write = shared_memory_write,
    .get_property = shared_memory_get_property,
    .set_property = nullptr,
};

bool open_shared_memory(process::Process& proc,
                        uint64_t name_ptr,
                        uint64_t length,
                        uint64_t,
                        Allocation& alloc) {
    process::Process* cur_proc = process::current();
    uint64_t cr3_value = 0;
    asm volatile("mov %%cr3, %0" : "=r"(cr3_value));
    if (cur_proc != nullptr && cur_proc != &proc) {
        log_message(LogLevel::Warn,
                    "SharedMemory: open called with pid=%u but current pid=%u (cr3=%016llx arg_cr3=%016llx)",
                    static_cast<unsigned int>(proc.pid),
                    static_cast<unsigned int>(cur_proc->pid),
                    static_cast<unsigned long long>(cr3_value),
                    static_cast<unsigned long long>(proc.cr3));
    } else if (cur_proc == nullptr) {
        log_message(LogLevel::Warn,
                    "SharedMemory: open sees null current process (target pid=%u cr3=%016llx)",
                    static_cast<unsigned int>(proc.pid),
                    static_cast<unsigned long long>(proc.cr3));
    }
    log_message(LogLevel::Debug,
                "SharedMemory: pid=%u open request name_ptr=%llx length=%llu",
                static_cast<unsigned int>(proc.pid),
                static_cast<unsigned long long>(name_ptr),
                static_cast<unsigned long long>(length));
    if (name_ptr == 0) {
        log_message(LogLevel::Warn,
                    "SharedMemory: pid=%u provided null name pointer",
                    static_cast<unsigned int>(proc.pid));
        return false;
    }
    const char* user_name = reinterpret_cast<const char*>(name_ptr);
    char name_buffer[kMaxNameLength];
    bool name_in_range = vm::is_user_range(name_ptr, 1);
    if (!vm::copy_user_string(user_name, name_buffer, sizeof(name_buffer))) {
        log_message(LogLevel::Warn,
                    "SharedMemory: pid=%u failed to copy name (in_range=%d cur_pid=%u cr3=%016llx)",
                    static_cast<unsigned int>(proc.pid),
                    name_in_range ? 1 : 0,
                    static_cast<unsigned int>(cur_proc != nullptr ? cur_proc->pid : 0),
                    static_cast<unsigned long long>(cr3_value));
        return false;
    }
    if (name_buffer[0] == '\0') {
        log_message(LogLevel::Warn,
                    "SharedMemory: pid=%u empty name",
                    static_cast<unsigned int>(proc.pid));
        return false;
    }
    size_t requested = static_cast<size_t>(length);
    SharedSegment* segment = nullptr;
    {
        SegmentGuard guard;
        segment = find_segment_locked(name_buffer);
        if (segment == nullptr) {
            segment = allocate_segment_locked(name_buffer, requested, proc);
            if (segment == nullptr) {
                return false;
            }
        } else if (requested != 0 && segment->region.length < requested) {
            return false;
        }
        ++segment->refcount;
    }

    auto rollback_segment_refcount = [&]() {
        SegmentGuard guard;
        if (segment->refcount > 0) {
            --segment->refcount;
        }
        if (segment->refcount == 0 && segment->in_use) {
            release_segment_pages(*segment);
            if (segment->owner_pid != 0 && segment->owner_cr3 != 0) {
                vm::release_user_region(segment->owner_cr3, segment->region);
            }
            reset_segment(*segment);
        }
    };

    if (!map_segment_into_process(*segment, proc, segment->region.base)) {
        rollback_segment_refcount();
        return false;
    }
    log_message(LogLevel::Debug,
                "SharedMemory: process %u opened segment '%s' base=%llx len=%zu ref=%u",
                static_cast<unsigned int>(proc.pid),
                segment->name,
                static_cast<unsigned long long>(segment->region.base),
                segment->region.length,
                segment->refcount);
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
