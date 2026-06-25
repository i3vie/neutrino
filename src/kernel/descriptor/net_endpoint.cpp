#include "../descriptor.hpp"

#include "../../lib/mem.hpp"
#include "../process.hpp"
#include "../scheduler.hpp"
#include "../vm.hpp"

namespace descriptor {

namespace descriptor_net_endpoint {

constexpr size_t kEndpointBufferSize = 65536;
constexpr size_t kMaxEndpoints = 64;
constexpr size_t kMaxEndpointHandles = kMaxEndpoints * 2;
constexpr size_t kMaxEndpointWaiters = 128;

enum class Role : uint32_t {
    App = 0,
    Service = 1,
};

struct EndpointWaiter {
    process::Process* proc;
    uint64_t user_address;
    uint64_t length;
    bool is_read;
    Role role;
    bool in_use;
    EndpointWaiter* next;
};

struct Ring {
    uint8_t buffer[kEndpointBufferSize];
    size_t head;
    size_t tail;
    size_t count;
};

struct NetEndpoint {
    Ring app_to_service;
    Ring service_to_app;
    size_t app_handles;
    size_t service_handles;
    size_t refcount;
    uint32_t id;
    bool in_use;
    volatile int lock;
    EndpointWaiter* app_read_waiters;
    EndpointWaiter* app_write_waiters;
    EndpointWaiter* service_read_waiters;
    EndpointWaiter* service_write_waiters;
};

struct EndpointHandle {
    NetEndpoint* endpoint;
    process::Process* owner;
    Role role;
    bool in_use;
};

NetEndpoint g_endpoints[kMaxEndpoints]{};
EndpointHandle g_handles[kMaxEndpointHandles]{};
EndpointWaiter g_waiters[kMaxEndpointWaiters]{};
uint32_t g_next_endpoint_id = 1;

inline size_t min_size(size_t a, size_t b) {
    return (a < b) ? a : b;
}

void lock_endpoint(NetEndpoint& endpoint) {
    while (__atomic_test_and_set(&endpoint.lock, __ATOMIC_ACQUIRE)) {
        asm volatile("pause");
    }
}

void unlock_endpoint(NetEndpoint& endpoint) {
    __atomic_clear(&endpoint.lock, __ATOMIC_RELEASE);
}

void reset_ring(Ring& ring) {
    ring.head = 0;
    ring.tail = 0;
    ring.count = 0;
    memset(ring.buffer, 0, sizeof(ring.buffer));
}

Ring& incoming_ring(NetEndpoint& endpoint, Role role) {
    return role == Role::App ? endpoint.service_to_app
                             : endpoint.app_to_service;
}

Ring& outgoing_ring(NetEndpoint& endpoint, Role role) {
    return role == Role::App ? endpoint.app_to_service
                             : endpoint.service_to_app;
}

size_t peer_handles(const NetEndpoint& endpoint, Role role) {
    return role == Role::App ? endpoint.service_handles
                             : endpoint.app_handles;
}

EndpointWaiter*& read_waiters(NetEndpoint& endpoint, Role role) {
    return role == Role::App ? endpoint.app_read_waiters
                             : endpoint.service_read_waiters;
}

EndpointWaiter*& write_waiters(NetEndpoint& endpoint, Role role) {
    return role == Role::App ? endpoint.app_write_waiters
                             : endpoint.service_write_waiters;
}

NetEndpoint* allocate_endpoint() {
    for (auto& endpoint : g_endpoints) {
        if (endpoint.in_use) {
            continue;
        }
        endpoint.in_use = true;
        reset_ring(endpoint.app_to_service);
        reset_ring(endpoint.service_to_app);
        endpoint.app_handles = 0;
        endpoint.service_handles = 0;
        endpoint.refcount = 0;
        endpoint.lock = 0;
        endpoint.app_read_waiters = nullptr;
        endpoint.app_write_waiters = nullptr;
        endpoint.service_read_waiters = nullptr;
        endpoint.service_write_waiters = nullptr;
        if (g_next_endpoint_id == 0) {
            g_next_endpoint_id = 1;
        }
        endpoint.id = g_next_endpoint_id++;
        return &endpoint;
    }
    return nullptr;
}

NetEndpoint* find_endpoint(uint32_t id) {
    if (id == 0) {
        return nullptr;
    }
    for (auto& endpoint : g_endpoints) {
        if (endpoint.in_use && endpoint.id == id) {
            return &endpoint;
        }
    }
    return nullptr;
}

EndpointHandle* allocate_handle(NetEndpoint* endpoint,
                                process::Process* owner,
                                Role role) {
    for (auto& handle : g_handles) {
        if (handle.in_use) {
            continue;
        }
        handle.in_use = true;
        handle.endpoint = endpoint;
        handle.owner = owner;
        handle.role = role;
        return &handle;
    }
    return nullptr;
}

void release_handle(EndpointHandle* handle) {
    if (handle == nullptr) {
        return;
    }
    handle->endpoint = nullptr;
    handle->owner = nullptr;
    handle->role = Role::App;
    handle->in_use = false;
}

EndpointWaiter* allocate_waiter() {
    for (auto& waiter : g_waiters) {
        if (waiter.in_use) {
            continue;
        }
        waiter.in_use = true;
        waiter.proc = nullptr;
        waiter.user_address = 0;
        waiter.length = 0;
        waiter.is_read = false;
        waiter.role = Role::App;
        waiter.next = nullptr;
        return &waiter;
    }
    return nullptr;
}

void release_waiter(EndpointWaiter* waiter) {
    if (waiter == nullptr) {
        return;
    }
    waiter->in_use = false;
    waiter->proc = nullptr;
    waiter->user_address = 0;
    waiter->length = 0;
    waiter->is_read = false;
    waiter->role = Role::App;
    waiter->next = nullptr;
}

void push_waiter(EndpointWaiter*& head, EndpointWaiter* waiter) {
    if (head == nullptr) {
        head = waiter;
        return;
    }
    EndpointWaiter* cur = head;
    while (cur->next != nullptr) {
        cur = cur->next;
    }
    cur->next = waiter;
}

void complete_waiter(EndpointWaiter* waiter, int64_t result) {
    if (waiter == nullptr || waiter->proc == nullptr) {
        release_waiter(waiter);
        return;
    }
    waiter->proc->context.rax = static_cast<uint64_t>(result);
    waiter->proc->state = process::State::Ready;
    waiter->proc->waiting_on = nullptr;
    scheduler::enqueue(waiter->proc);
    release_waiter(waiter);
}

int64_t ring_copy_out_to_user(Ring& ring,
                              process::Process& proc,
                              uint64_t user_address,
                              size_t max_bytes) {
    if (user_address == 0 || proc.cr3 == 0) {
        return -1;
    }
    size_t copied = 0;
    while (copied < max_bytes && ring.count > 0) {
        size_t chunk = min_size(
            min_size(max_bytes - copied, ring.count),
            kEndpointBufferSize - ring.head);
        if (!vm::copy_to_user(proc.cr3,
                              user_address + copied,
                              ring.buffer + ring.head,
                              chunk)) {
            return copied > 0 ? static_cast<int64_t>(copied) : -1;
        }
        ring.head = (ring.head + chunk) % kEndpointBufferSize;
        ring.count -= chunk;
        copied += chunk;
    }
    return static_cast<int64_t>(copied);
}

int64_t ring_copy_in_from_user(Ring& ring,
                               process::Process& proc,
                               uint64_t user_address,
                               size_t max_bytes) {
    if (user_address == 0 || proc.cr3 == 0) {
        return -1;
    }
    size_t copied = 0;
    while (copied < max_bytes && ring.count < kEndpointBufferSize) {
        size_t space = kEndpointBufferSize - ring.count;
        size_t chunk = min_size(
            min_size(max_bytes - copied, space),
            kEndpointBufferSize - ring.tail);
        if (!vm::copy_from_user(proc.cr3,
                                ring.buffer + ring.tail,
                                user_address + copied,
                                chunk)) {
            return copied > 0 ? static_cast<int64_t>(copied) : -1;
        }
        ring.tail = (ring.tail + chunk) % kEndpointBufferSize;
        ring.count += chunk;
        copied += chunk;
    }
    return static_cast<int64_t>(copied);
}

void wake_read_waiters_locked(NetEndpoint& endpoint, Role role);
void wake_write_waiters_locked(NetEndpoint& endpoint, Role role);

void wake_read_waiters_locked(NetEndpoint& endpoint, Role role) {
    EndpointWaiter*& waiters = read_waiters(endpoint, role);
    Ring& ring = incoming_ring(endpoint, role);
    while (waiters != nullptr) {
        EndpointWaiter* waiter = waiters;
        if (ring.count == 0 && peer_handles(endpoint, role) == 0) {
            waiters = waiter->next;
            waiter->next = nullptr;
            complete_waiter(waiter, 0);
            continue;
        }
        if (ring.count == 0) {
            break;
        }
        if (waiter->proc == nullptr) {
            waiters = waiter->next;
            waiter->next = nullptr;
            complete_waiter(waiter, -1);
            continue;
        }
        int64_t copied =
            ring_copy_out_to_user(ring,
                                  *waiter->proc,
                                  waiter->user_address,
                                  static_cast<size_t>(waiter->length));
        waiters = waiter->next;
        waiter->next = nullptr;
        complete_waiter(waiter, copied);
        wake_write_waiters_locked(endpoint,
                                  role == Role::App ? Role::Service
                                                    : Role::App);
    }
}

void wake_write_waiters_locked(NetEndpoint& endpoint, Role role) {
    EndpointWaiter*& waiters = write_waiters(endpoint, role);
    Ring& ring = outgoing_ring(endpoint, role);
    while (waiters != nullptr) {
        EndpointWaiter* waiter = waiters;
        if (peer_handles(endpoint, role) == 0) {
            waiters = waiter->next;
            waiter->next = nullptr;
            complete_waiter(waiter, -1);
            continue;
        }
        if (ring.count >= kEndpointBufferSize) {
            break;
        }
        if (waiter->proc == nullptr) {
            waiters = waiter->next;
            waiter->next = nullptr;
            complete_waiter(waiter, -1);
            continue;
        }
        int64_t copied =
            ring_copy_in_from_user(ring,
                                   *waiter->proc,
                                   waiter->user_address,
                                   static_cast<size_t>(waiter->length));
        waiters = waiter->next;
        waiter->next = nullptr;
        complete_waiter(waiter, copied);
        wake_read_waiters_locked(endpoint,
                                 role == Role::App ? Role::Service
                                                   : Role::App);
        if (ring.count >= kEndpointBufferSize) {
            break;
        }
    }
}

int64_t endpoint_read(process::Process& proc,
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
    auto* handle = static_cast<EndpointHandle*>(entry.subsystem_data);
    if (handle == nullptr || !handle->in_use || handle->endpoint == nullptr) {
        return -1;
    }
    NetEndpoint& endpoint = *handle->endpoint;
    if (!endpoint.in_use || user_address == 0) {
        return -1;
    }
    bool async = has_flag(entry.flags, Flag::Async);
    size_t requested = static_cast<size_t>(length);

    lock_endpoint(endpoint);
    Ring& ring = incoming_ring(endpoint, handle->role);
    if (ring.count > 0) {
        int64_t copied = ring_copy_out_to_user(ring, proc, user_address, requested);
        wake_write_waiters_locked(endpoint,
                                  handle->role == Role::App ? Role::Service
                                                            : Role::App);
        unlock_endpoint(endpoint);
        descriptor::wake_waiters();
        return copied;
    }
    if (peer_handles(endpoint, handle->role) == 0) {
        unlock_endpoint(endpoint);
        return 0;
    }
    if (async) {
        unlock_endpoint(endpoint);
        return kWouldBlock;
    }

    EndpointWaiter* waiter = allocate_waiter();
    if (waiter == nullptr) {
        unlock_endpoint(endpoint);
        return -1;
    }
    waiter->proc = &proc;
    waiter->user_address = user_address;
    waiter->length = length;
    waiter->is_read = true;
    waiter->role = handle->role;
    waiter->next = nullptr;
    push_waiter(read_waiters(endpoint, handle->role), waiter);
    proc.state = process::State::Blocked;
    proc.waiting_on = &endpoint;
    unlock_endpoint(endpoint);
    return kWouldBlock;
}

int64_t endpoint_write(process::Process& proc,
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
    auto* handle = static_cast<EndpointHandle*>(entry.subsystem_data);
    if (handle == nullptr || !handle->in_use || handle->endpoint == nullptr) {
        return -1;
    }
    NetEndpoint& endpoint = *handle->endpoint;
    if (!endpoint.in_use || user_address == 0) {
        return -1;
    }
    bool async = has_flag(entry.flags, Flag::Async);
    size_t requested = static_cast<size_t>(length);

    lock_endpoint(endpoint);
    if (peer_handles(endpoint, handle->role) == 0) {
        unlock_endpoint(endpoint);
        return -1;
    }
    Ring& ring = outgoing_ring(endpoint, handle->role);
    if (ring.count < kEndpointBufferSize) {
        int64_t copied = ring_copy_in_from_user(ring, proc, user_address, requested);
        wake_read_waiters_locked(endpoint,
                                 handle->role == Role::App ? Role::Service
                                                           : Role::App);
        unlock_endpoint(endpoint);
        descriptor::wake_waiters();
        return copied;
    }
    if (async) {
        unlock_endpoint(endpoint);
        return kWouldBlock;
    }

    EndpointWaiter* waiter = allocate_waiter();
    if (waiter == nullptr) {
        unlock_endpoint(endpoint);
        return -1;
    }
    waiter->proc = &proc;
    waiter->user_address = user_address;
    waiter->length = length;
    waiter->is_read = false;
    waiter->role = handle->role;
    waiter->next = nullptr;
    push_waiter(write_waiters(endpoint, handle->role), waiter);
    proc.state = process::State::Blocked;
    proc.waiting_on = &endpoint;
    unlock_endpoint(endpoint);
    return kWouldBlock;
}

int endpoint_get_property(DescriptorEntry& entry,
                          uint32_t property,
                          void* out,
                          size_t size) {
    if (property !=
        static_cast<uint32_t>(descriptor_defs::Property::NetEndpointInfo)) {
        return -1;
    }
    if (out == nullptr || size < sizeof(descriptor_defs::NetEndpointInfo)) {
        return -1;
    }
    auto* handle = static_cast<EndpointHandle*>(entry.subsystem_data);
    if (handle == nullptr || !handle->in_use || handle->endpoint == nullptr ||
        !handle->endpoint->in_use) {
        return -1;
    }
    auto* info = reinterpret_cast<descriptor_defs::NetEndpointInfo*>(out);
    info->id = handle->endpoint->id;
    info->flags = static_cast<uint32_t>(entry.flags & 0xFFFFFFFFu);
    info->role = static_cast<uint32_t>(handle->role);
    info->reserved = 0;
    return 0;
}

void drop_waiters_for_owner_locked(NetEndpoint& endpoint, process::Process* owner) {
    EndpointWaiter** lists[] = {
        &endpoint.app_read_waiters,
        &endpoint.app_write_waiters,
        &endpoint.service_read_waiters,
        &endpoint.service_write_waiters,
    };
    for (auto** list : lists) {
        EndpointWaiter* prev = nullptr;
        EndpointWaiter* cur = *list;
        while (cur != nullptr) {
            EndpointWaiter* next = cur->next;
            if (cur->proc == owner) {
                if (prev == nullptr) {
                    *list = next;
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
}

void close_endpoint(DescriptorEntry& entry) {
    auto* handle = static_cast<EndpointHandle*>(entry.subsystem_data);
    if (handle == nullptr || !handle->in_use || handle->endpoint == nullptr) {
        return;
    }
    NetEndpoint& endpoint = *handle->endpoint;
    bool notify_waiters = false;
    lock_endpoint(endpoint);
    if (endpoint.refcount > 0) {
        --endpoint.refcount;
    }
    if (handle->role == Role::App && endpoint.app_handles > 0) {
        --endpoint.app_handles;
    }
    if (handle->role == Role::Service && endpoint.service_handles > 0) {
        --endpoint.service_handles;
    }

    wake_read_waiters_locked(endpoint, Role::App);
    wake_write_waiters_locked(endpoint, Role::App);
    wake_read_waiters_locked(endpoint, Role::Service);
    wake_write_waiters_locked(endpoint, Role::Service);
    notify_waiters = true;
    drop_waiters_for_owner_locked(endpoint, handle->owner);

    bool empty = endpoint.refcount == 0;
    if (empty) {
        endpoint.in_use = false;
        reset_ring(endpoint.app_to_service);
        reset_ring(endpoint.service_to_app);
        endpoint.app_handles = 0;
        endpoint.service_handles = 0;
        endpoint.app_read_waiters = nullptr;
        endpoint.app_write_waiters = nullptr;
        endpoint.service_read_waiters = nullptr;
        endpoint.service_write_waiters = nullptr;
        endpoint.lock = 0;
    }
    unlock_endpoint(endpoint);
    release_handle(handle);
    if (notify_waiters) {
        descriptor::wake_waiters();
    }
}

bool query_wait(DescriptorEntry& entry, uint32_t events, uint32_t& revents) {
    revents = 0;
    auto* handle = static_cast<EndpointHandle*>(entry.subsystem_data);
    if (handle == nullptr || !handle->in_use || handle->endpoint == nullptr) {
        return false;
    }
    NetEndpoint& endpoint = *handle->endpoint;
    if (!endpoint.in_use) {
        return false;
    }

    lock_endpoint(endpoint);
    Ring& incoming = incoming_ring(endpoint, handle->role);
    Ring& outgoing = outgoing_ring(endpoint, handle->role);
    size_t peers = peer_handles(endpoint, handle->role);
    if ((events & descriptor_defs::kWaitRead) != 0 &&
        (incoming.count > 0 || peers == 0)) {
        revents |= descriptor_defs::kWaitRead;
    }
    if ((events & descriptor_defs::kWaitWrite) != 0 &&
        peers != 0 &&
        outgoing.count < kEndpointBufferSize) {
        revents |= descriptor_defs::kWaitWrite;
    }
    unlock_endpoint(endpoint);
    return true;
}

const Ops kEndpointOps{
    .read = endpoint_read,
    .write = endpoint_write,
    .get_property = endpoint_get_property,
    .set_property = nullptr,
};

bool open_endpoint(process::Process& proc,
                   uint64_t flags,
                   uint64_t existing_id,
                   uint64_t open_context,
                   Allocation& alloc) {
    bool want_service =
        (open_context & descriptor_defs::kNetEndpointOpenService) != 0;
    Role role = want_service ? Role::Service : Role::App;
    bool async = (flags & static_cast<uint64_t>(Flag::Async)) != 0;
    bool created = existing_id == 0;
    NetEndpoint* endpoint =
        created ? allocate_endpoint()
                : find_endpoint(static_cast<uint32_t>(existing_id));
    if (endpoint == nullptr || !endpoint->in_use) {
        return false;
    }
    EndpointHandle* handle = allocate_handle(endpoint, &proc, role);
    if (handle == nullptr) {
        if (created) {
            endpoint->in_use = false;
        }
        return false;
    }

    lock_endpoint(*endpoint);
    ++endpoint->refcount;
    if (role == Role::App) {
        ++endpoint->app_handles;
    } else {
        ++endpoint->service_handles;
    }
    unlock_endpoint(*endpoint);
    descriptor::wake_waiters();

    uint64_t descriptor_flags =
        static_cast<uint64_t>(Flag::Readable) |
        static_cast<uint64_t>(Flag::Writable) |
        static_cast<uint64_t>(Flag::CapStream);
    if (async) {
        descriptor_flags |= static_cast<uint64_t>(Flag::Async);
    }

    alloc.type = kTypeNetEndpoint;
    alloc.flags = descriptor_flags;
    alloc.extended_flags = 0;
    alloc.has_extended_flags = false;
    alloc.object = endpoint;
    alloc.subsystem_data = handle;
    alloc.name = "net-endpoint";
    alloc.ops = &kEndpointOps;
    alloc.close = close_endpoint;
    return true;
}

}  // namespace descriptor_net_endpoint

bool register_net_endpoint_descriptor() {
    return register_type(kTypeNetEndpoint,
                         descriptor_net_endpoint::open_endpoint,
                         &descriptor_net_endpoint::kEndpointOps);
}

}  // namespace descriptor
