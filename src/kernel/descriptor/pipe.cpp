#include "../descriptor.hpp"

#include "../../lib/mem.hpp"
#include "../process.hpp"
#include "../scheduler.hpp"

namespace descriptor {

namespace descriptor_pipe {

constexpr size_t kPipeBufferSize = 4096;
constexpr size_t kMaxPipes = 64;
constexpr size_t kMaxPipeWaiters = 128;

struct PipeWaiter {
    process::Process* proc;
    uint64_t user_address;
    uint64_t length;
    bool is_read;
    bool in_use;
    PipeWaiter* next;
};

struct Pipe {
    uint8_t buffer[kPipeBufferSize];
    size_t head;
    size_t tail;
    size_t count;
    size_t reader_count;
    size_t writer_count;
    size_t refcount;
    bool in_use;
    volatile int lock;
    PipeWaiter* read_waiters;
    PipeWaiter* write_waiters;
    uint32_t id;
};

struct PipeEndpoint {
    Pipe* pipe;
    process::Process* owner;
    bool can_read;
    bool can_write;
    bool in_use;
};

Pipe g_pipes[kMaxPipes]{};
PipeEndpoint g_pipe_endpoints[kMaxPipes * 2]{};
PipeWaiter g_pipe_waiters[kMaxPipeWaiters]{};
uint32_t g_next_pipe_id = 1;

inline size_t min_size(size_t a, size_t b) {
    return (a < b) ? a : b;
}

void lock_pipe(Pipe& pipe) {
    while (__atomic_test_and_set(&pipe.lock, __ATOMIC_ACQUIRE)) {
        asm volatile("pause");
    }
}

void unlock_pipe(Pipe& pipe) {
    __atomic_clear(&pipe.lock, __ATOMIC_RELEASE);
}

Pipe* allocate_pipe() {
    for (auto& pipe : g_pipes) {
        if (pipe.in_use) {
            continue;
        }
        pipe.in_use = true;
        pipe.head = 0;
        pipe.tail = 0;
        pipe.count = 0;
        pipe.reader_count = 0;
        pipe.writer_count = 0;
        pipe.refcount = 0;
        pipe.read_waiters = nullptr;
        pipe.write_waiters = nullptr;
        pipe.lock = 0;
        if (g_next_pipe_id == 0) {
            g_next_pipe_id = 1;
        }
        pipe.id = g_next_pipe_id++;
        memset(pipe.buffer, 0, sizeof(pipe.buffer));
        return &pipe;
    }
    return nullptr;
}

PipeEndpoint* allocate_pipe_endpoint(Pipe* pipe,
                                     process::Process* owner,
                                     bool can_read,
                                     bool can_write) {
    for (auto& endpoint : g_pipe_endpoints) {
        if (endpoint.in_use) {
            continue;
        }
        endpoint.in_use = true;
        endpoint.pipe = pipe;
        endpoint.owner = owner;
        endpoint.can_read = can_read;
        endpoint.can_write = can_write;
        return &endpoint;
    }
    return nullptr;
}

Pipe* find_pipe_by_id(uint32_t id) {
    if (id == 0) {
        return nullptr;
    }
    for (auto& pipe : g_pipes) {
        if (!pipe.in_use) {
            continue;
        }
        if (pipe.id == id) {
            return &pipe;
        }
    }
    return nullptr;
}

void release_pipe_endpoint(PipeEndpoint* endpoint) {
    if (endpoint == nullptr) {
        return;
    }
    endpoint->pipe = nullptr;
    endpoint->owner = nullptr;
    endpoint->can_read = false;
    endpoint->can_write = false;
    endpoint->in_use = false;
}

PipeWaiter* allocate_pipe_waiter() {
    for (auto& waiter : g_pipe_waiters) {
        if (waiter.in_use) {
            continue;
        }
        waiter.in_use = true;
        waiter.proc = nullptr;
        waiter.user_address = 0;
        waiter.length = 0;
        waiter.is_read = false;
        waiter.next = nullptr;
        return &waiter;
    }
    return nullptr;
}

void release_pipe_waiter(PipeWaiter* waiter) {
    if (waiter == nullptr) {
        return;
    }
    waiter->in_use = false;
    waiter->proc = nullptr;
    waiter->user_address = 0;
    waiter->length = 0;
    waiter->is_read = false;
    waiter->next = nullptr;
}

void push_waiter(PipeWaiter*& head, PipeWaiter* waiter) {
    if (head == nullptr) {
        head = waiter;
        return;
    }
    PipeWaiter* cur = head;
    while (cur->next != nullptr) {
        cur = cur->next;
    }
    cur->next = waiter;
}

void complete_waiter(PipeWaiter* waiter, int64_t result) {
    if (waiter == nullptr || waiter->proc == nullptr) {
        release_pipe_waiter(waiter);
        return;
    }
    waiter->proc->context.rax = static_cast<uint64_t>(result);
    waiter->proc->state = process::State::Ready;
    waiter->proc->waiting_on = nullptr;
    scheduler::enqueue(waiter->proc);
    release_pipe_waiter(waiter);
}

size_t pipe_copy_out(Pipe& pipe, uint8_t* dest, size_t max_bytes) {
    size_t copied = 0;
    while (copied < max_bytes && pipe.count > 0) {
        size_t chunk = min_size(
            min_size(max_bytes - copied, pipe.count),
            kPipeBufferSize - pipe.head);
        for (size_t i = 0; i < chunk; ++i) {
            dest[copied + i] = pipe.buffer[pipe.head + i];
        }
        pipe.head = (pipe.head + chunk) % kPipeBufferSize;
        pipe.count -= chunk;
        copied += chunk;
    }
    return copied;
}

size_t pipe_copy_in(Pipe& pipe, const uint8_t* src, size_t max_bytes) {
    size_t copied = 0;
    while (copied < max_bytes && pipe.count < kPipeBufferSize) {
        size_t space = kPipeBufferSize - pipe.count;
        size_t chunk = min_size(
            min_size(max_bytes - copied, space),
            kPipeBufferSize - pipe.tail);
        for (size_t i = 0; i < chunk; ++i) {
            pipe.buffer[pipe.tail + i] = src[copied + i];
        }
        pipe.tail = (pipe.tail + chunk) % kPipeBufferSize;
        pipe.count += chunk;
        copied += chunk;
    }
    return copied;
}

void drop_waiters_for_owner_locked(Pipe& pipe, process::Process* owner) {
    PipeWaiter* prev = nullptr;
    PipeWaiter* cur = pipe.read_waiters;
    while (cur != nullptr) {
        PipeWaiter* next = cur->next;
        if (cur->proc == owner) {
            if (prev == nullptr) {
                pipe.read_waiters = next;
            } else {
                prev->next = next;
            }
            cur->next = nullptr;
            complete_waiter(cur, -1);
        } else {
            prev = cur;
        }
        cur = next;
    }

    prev = nullptr;
    cur = pipe.write_waiters;
    while (cur != nullptr) {
        PipeWaiter* next = cur->next;
        if (cur->proc == owner) {
            if (prev == nullptr) {
                pipe.write_waiters = next;
            } else {
                prev->next = next;
            }
            cur->next = nullptr;
            complete_waiter(cur, -1);
        } else {
            prev = cur;
        }
        cur = next;
    }
}

void wake_read_waiters_locked(Pipe& pipe) {
    while (pipe.read_waiters != nullptr) {
        PipeWaiter* waiter = pipe.read_waiters;
        if (pipe.count == 0 && pipe.writer_count == 0) {
            pipe.read_waiters = waiter->next;
            waiter->next = nullptr;
            complete_waiter(waiter, 0);
            continue;
        }
        if (pipe.count == 0) {
            break;
        }
        uint8_t* dest = reinterpret_cast<uint8_t*>(waiter->user_address);
        if (dest == nullptr) {
            pipe.read_waiters = waiter->next;
            waiter->next = nullptr;
            complete_waiter(waiter, -1);
            continue;
        }
        size_t copied = pipe_copy_out(pipe, dest, static_cast<size_t>(waiter->length));
        pipe.read_waiters = waiter->next;
        waiter->next = nullptr;
        complete_waiter(waiter, static_cast<int64_t>(copied));
    }
}

void wake_write_waiters_locked(Pipe& pipe) {
    while (pipe.write_waiters != nullptr) {
        PipeWaiter* waiter = pipe.write_waiters;
        if (pipe.reader_count == 0) {
            pipe.write_waiters = waiter->next;
            waiter->next = nullptr;
            complete_waiter(waiter, -1);
            continue;
        }
        if (pipe.count >= kPipeBufferSize) {
            break;
        }
        const uint8_t* src = reinterpret_cast<const uint8_t*>(waiter->user_address);
        if (src == nullptr) {
            pipe.write_waiters = waiter->next;
            waiter->next = nullptr;
            complete_waiter(waiter, -1);
            continue;
        }
        size_t copied = pipe_copy_in(pipe, src, static_cast<size_t>(waiter->length));
        pipe.write_waiters = waiter->next;
        waiter->next = nullptr;
        complete_waiter(waiter, static_cast<int64_t>(copied));
        if (pipe.count >= kPipeBufferSize) {
            break;
        }
    }
}

int64_t pipe_read(process::Process&,
                  DescriptorEntry& entry,
                  uint64_t user_address,
                  uint64_t length,
                  uint64_t offset) {
    if (offset != 0) {
        return -1;
    }
    if (length == 0) {
        return 0;
    }
    auto* endpoint = static_cast<PipeEndpoint*>(entry.subsystem_data);
    if (endpoint == nullptr || !endpoint->in_use) {
        return -1;
    }
    auto* pipe = endpoint->pipe;
    if (pipe == nullptr || !pipe->in_use || !endpoint->can_read) {
        return -1;
    }
    auto* dest = reinterpret_cast<uint8_t*>(user_address);
    if (dest == nullptr && length != 0) {
        return -1;
    }

    size_t requested = static_cast<size_t>(length);
    size_t read_count = 0;
    bool async = has_flag(entry.flags, Flag::Async);

    lock_pipe(*pipe);

    if (pipe->count > 0) {
        read_count = pipe_copy_out(*pipe, dest, requested);
    }

    if (read_count > 0 || async) {
        wake_write_waiters_locked(*pipe);
        unlock_pipe(*pipe);
        return static_cast<int64_t>(read_count);
    }

    if (pipe->writer_count == 0) {
        unlock_pipe(*pipe);
        return 0;
    }

    PipeWaiter* waiter = allocate_pipe_waiter();
    if (waiter == nullptr) {
        unlock_pipe(*pipe);
        return -1;
    }
    waiter->proc = process::current();
    waiter->user_address = user_address;
    waiter->length = length;
    waiter->is_read = true;
    waiter->next = nullptr;

    push_waiter(pipe->read_waiters, waiter);

    process::Process* proc = process::current();
    if (proc != nullptr) {
        proc->state = process::State::Blocked;
        proc->waiting_on = pipe;
    }

    unlock_pipe(*pipe);
    return kWouldBlock;
}

int64_t pipe_write(process::Process&,
                   DescriptorEntry& entry,
                   uint64_t user_address,
                   uint64_t length,
                   uint64_t offset) {
    if (offset != 0) {
        return -1;
    }
    if (length == 0) {
        return 0;
    }
    auto* endpoint = static_cast<PipeEndpoint*>(entry.subsystem_data);
    if (endpoint == nullptr || !endpoint->in_use) {
        return -1;
    }
    auto* pipe = endpoint->pipe;
    if (pipe == nullptr || !pipe->in_use || !endpoint->can_write) {
        return -1;
    }
    const auto* src = reinterpret_cast<const uint8_t*>(user_address);
    if (src == nullptr && length != 0) {
        return -1;
    }

    size_t requested = static_cast<size_t>(length);
    size_t written = 0;
    bool async = has_flag(entry.flags, Flag::Async);

    lock_pipe(*pipe);

    if (pipe->reader_count == 0) {
        unlock_pipe(*pipe);
        return -1;
    }

    if (pipe->count < kPipeBufferSize) {
        written = pipe_copy_in(*pipe, src, requested);
    }

    if (written > 0 || async) {
        wake_read_waiters_locked(*pipe);
        unlock_pipe(*pipe);
        return static_cast<int64_t>(written);
    }

    PipeWaiter* waiter = allocate_pipe_waiter();
    if (waiter == nullptr) {
        unlock_pipe(*pipe);
        return -1;
    }
    waiter->proc = process::current();
    waiter->user_address = user_address;
    waiter->length = length;
    waiter->is_read = false;
    waiter->next = nullptr;

    push_waiter(pipe->write_waiters, waiter);

    process::Process* proc = process::current();
    if (proc != nullptr) {
        proc->state = process::State::Blocked;
        proc->waiting_on = pipe;
    }

    unlock_pipe(*pipe);
    return kWouldBlock;
}

int pipe_get_property(DescriptorEntry& entry,
                      uint32_t property,
                      void* out,
                      size_t size) {
    if (property !=
        static_cast<uint32_t>(descriptor_defs::Property::PipeInfo)) {
        return -1;
    }
    auto* endpoint = static_cast<PipeEndpoint*>(entry.subsystem_data);
    if (endpoint == nullptr || !endpoint->in_use) {
        return -1;
    }
    Pipe* pipe = endpoint->pipe;
    if (pipe == nullptr || !pipe->in_use) {
        return -1;
    }
    if (out == nullptr || size < sizeof(descriptor_defs::PipeInfo)) {
        return -1;
    }
    auto* info = reinterpret_cast<descriptor_defs::PipeInfo*>(out);
    info->id = pipe->id;
    info->flags = static_cast<uint32_t>(entry.flags & 0xFFFFFFFFu);
    return 0;
}

void close_pipe(DescriptorEntry& entry) {
    auto* endpoint = static_cast<PipeEndpoint*>(entry.subsystem_data);
    if (endpoint == nullptr || !endpoint->in_use) {
        return;
    }
    Pipe* pipe = endpoint->pipe;
    if (pipe == nullptr || !pipe->in_use) {
        release_pipe_endpoint(endpoint);
        return;
    }

    lock_pipe(*pipe);

    if (pipe->refcount > 0) {
        --pipe->refcount;
    }
    if (endpoint->can_read && pipe->reader_count > 0) {
        --pipe->reader_count;
    }
    if (endpoint->can_write && pipe->writer_count > 0) {
        --pipe->writer_count;
    }

    if (pipe->writer_count == 0) {
        wake_read_waiters_locked(*pipe);
    }
    if (pipe->reader_count == 0) {
        wake_write_waiters_locked(*pipe);
    }

    drop_waiters_for_owner_locked(*pipe, endpoint->owner);

    bool empty = pipe->refcount == 0;
    if (empty) {
        while (pipe->read_waiters != nullptr) {
            PipeWaiter* w = pipe->read_waiters;
            pipe->read_waiters = w->next;
            w->next = nullptr;
            complete_waiter(w, -1);
        }
        while (pipe->write_waiters != nullptr) {
            PipeWaiter* w = pipe->write_waiters;
            pipe->write_waiters = w->next;
            w->next = nullptr;
            complete_waiter(w, -1);
        }
        pipe->in_use = false;
        pipe->head = 0;
        pipe->tail = 0;
        pipe->count = 0;
        pipe->reader_count = 0;
        pipe->writer_count = 0;
        pipe->read_waiters = nullptr;
        pipe->write_waiters = nullptr;
        pipe->lock = 0;
    }
    unlock_pipe(*pipe);

    release_pipe_endpoint(endpoint);
}

const Ops kPipeOps{
    .read = pipe_read,
    .write = pipe_write,
    .get_property = pipe_get_property,
    .set_property = nullptr,
};

bool open_pipe(process::Process&,
               uint64_t flags,
               uint64_t existing_id,
               uint64_t,
               Allocation& alloc) {
    bool want_read = (flags & static_cast<uint64_t>(Flag::Readable)) != 0;
    bool want_write = (flags & static_cast<uint64_t>(Flag::Writable)) != 0;
    bool async = (flags & static_cast<uint64_t>(Flag::Async)) != 0;
    if (!want_read && !want_write) {
        return false;
    }

    bool created_pipe = (existing_id == 0);
    Pipe* pipe = created_pipe ? allocate_pipe()
                              : find_pipe_by_id(static_cast<uint32_t>(existing_id));
    if (pipe == nullptr || !pipe->in_use) {
        return false;
    }

    PipeEndpoint* endpoint =
        allocate_pipe_endpoint(pipe, process::current(), want_read, want_write);
    if (endpoint == nullptr) {
        if (created_pipe) {
            pipe->in_use = false;
        }
        return false;
    }

    lock_pipe(*pipe);
    pipe->refcount++;
    if (want_read) {
        pipe->reader_count++;
    }
    if (want_write) {
        pipe->writer_count++;
    }
    unlock_pipe(*pipe);

    uint64_t descriptor_flags = 0;
    if (want_read) {
        descriptor_flags |= static_cast<uint64_t>(Flag::Readable);
    }
    if (want_write) {
        descriptor_flags |= static_cast<uint64_t>(Flag::Writable);
    }
    if (async) {
        descriptor_flags |= static_cast<uint64_t>(Flag::Async);
    }

    alloc.type = kTypePipe;
    alloc.flags = descriptor_flags;
    alloc.extended_flags = 0;
    alloc.has_extended_flags = false;
    alloc.object = pipe;
    alloc.subsystem_data = endpoint;
    alloc.name = "pipe";
    alloc.ops = &kPipeOps;
    alloc.close = close_pipe;
    return true;
}

}  // namespace descriptor_pipe

bool register_pipe_descriptor() {
    return register_type(kTypePipe, descriptor_pipe::open_pipe, &descriptor_pipe::kPipeOps);
}

}  // namespace descriptor
