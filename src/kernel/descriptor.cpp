#include "descriptor.hpp"

#include "process.hpp"

namespace descriptor {

namespace {

struct TypeRegistration {
    uint32_t type;
    OpenFn open;
    const Ops* ops;
    bool used;
};

constexpr size_t kMaxRegisteredTypes = 32;
TypeRegistration g_type_registry[kMaxRegisteredTypes];

TypeRegistration* find_registration(uint32_t type) {
    for (size_t i = 0; i < kMaxRegisteredTypes; ++i) {
        if (g_type_registry[i].used && g_type_registry[i].type == type) {
            return &g_type_registry[i];
        }
    }
    return nullptr;
}

const TypeRegistration* find_registration(uint32_t type, bool) {
    return find_registration(type);
}

Entry* get_entry(Table& table, uint32_t handle) {
    if (handle >= kMaxDescriptors) {
        return nullptr;
    }
    Entry& entry = table.entries[handle];
    if (!entry.in_use) {
        return nullptr;
    }
    return &entry;
}

}  // namespace

bool register_type(uint32_t type, OpenFn open, const Ops* ops) {
    if (type > kTypeMask) {
        return false;
    }
    if (find_registration(type) != nullptr) {
        return false;
    }
    for (size_t i = 0; i < kMaxRegisteredTypes; ++i) {
        if (!g_type_registry[i].used) {
            g_type_registry[i] = TypeRegistration{
                .type = type,
                .open = open,
                .ops = ops,
                .used = true,
            };
            return true;
        }
    }
    return false;
}

void init() {
    for (size_t i = 0; i < kMaxRegisteredTypes; ++i) {
        g_type_registry[i].used = false;
        g_type_registry[i].type = 0;
        g_type_registry[i].open = nullptr;
        g_type_registry[i].ops = nullptr;
    }
}

void init_table(Table& table) {
    for (size_t i = 0; i < kMaxDescriptors; ++i) {
        table.entries[i].info = 0;
        table.entries[i].object = nullptr;
        table.entries[i].close = nullptr;
        table.entries[i].ops = nullptr;
        table.entries[i].in_use = false;
    }
}

void destroy_table(process::Process& proc, Table& table) {
    (void)proc;
    for (size_t i = 0; i < kMaxDescriptors; ++i) {
        if (!table.entries[i].in_use) {
            continue;
        }
        if (table.entries[i].close != nullptr) {
            table.entries[i].close(table.entries[i].object);
        }
        table.entries[i].info = 0;
        table.entries[i].object = nullptr;
        table.entries[i].close = nullptr;
        table.entries[i].ops = nullptr;
        table.entries[i].in_use = false;
    }
}

int32_t install(process::Process& proc,
                Table& table,
                const Allocation& alloc) {
    (void)proc;
    for (size_t i = 0; i < kMaxDescriptors; ++i) {
        if (table.entries[i].in_use) {
            continue;
        }
        table.entries[i].info = alloc.info;
        table.entries[i].object = alloc.object;
        table.entries[i].close = alloc.close;
        table.entries[i].ops = alloc.ops;
        table.entries[i].in_use = true;
        return static_cast<int32_t>(i);
    }
    return -1;
}

int32_t open(process::Process& proc,
             Table& table,
             uint32_t type,
             uint64_t arg0,
             uint64_t arg1,
             uint64_t arg2) {
    TypeRegistration* reg = find_registration(type);
    if (reg == nullptr || reg->open == nullptr) {
        return -1;
    }
    Allocation alloc{
        .info = make_info(type, 0),
        .object = nullptr,
        .close = nullptr,
        .ops = reg->ops,
    };
    if (!reg->open(proc, arg0, arg1, arg2, alloc)) {
        if (alloc.close != nullptr && alloc.object != nullptr) {
            alloc.close(alloc.object);
        }
        return -1;
    }
    if (info_type(alloc.info) != type) {
        alloc.info = make_info(type, info_capabilities(alloc.info));
    }
    if (alloc.ops == nullptr) {
        alloc.ops = reg->ops;
    }
    return install(proc, table, alloc);
}

uint32_t query(const Table& table, uint32_t handle, bool* ok) {
    if (handle >= kMaxDescriptors) {
        if (ok != nullptr) {
            *ok = false;
        }
        return 0;
    }
    const Entry& entry = table.entries[handle];
    if (!entry.in_use) {
        if (ok != nullptr) {
            *ok = false;
        }
        return 0;
    }
    if (ok != nullptr) {
        *ok = true;
    }
    return entry.info;
}

int64_t read(process::Process& proc,
             Table& table,
             uint32_t handle,
             uint64_t user_address,
             uint64_t length,
             uint64_t offset) {
    Entry* entry = get_entry(table, handle);
    if (entry == nullptr) {
        return -1;
    }
    if (!has_capability(entry->info, CapabilityReadable)) {
        return -1;
    }
    if (entry->ops == nullptr || entry->ops->read == nullptr) {
        return -1;
    }
    return entry->ops->read(proc, *entry, user_address, length, offset);
}

int64_t write(process::Process& proc,
              Table& table,
              uint32_t handle,
              uint64_t user_address,
              uint64_t length,
              uint64_t offset) {
    Entry* entry = get_entry(table, handle);
    if (entry == nullptr) {
        return -1;
    }
    if (!has_capability(entry->info, CapabilityWritable)) {
        return -1;
    }
    if (entry->ops == nullptr || entry->ops->write == nullptr) {
        return -1;
    }
    return entry->ops->write(proc, *entry, user_address, length, offset);
}

bool close(process::Process& proc, Table& table, uint32_t handle) {
    (void)proc;
    Entry* entry = get_entry(table, handle);
    if (entry == nullptr) {
        return false;
    }
    if (entry->close != nullptr) {
        entry->close(entry->object);
    }
    entry->info = 0;
    entry->object = nullptr;
    entry->close = nullptr;
    entry->ops = nullptr;
    entry->in_use = false;
    return true;
}

}  // namespace descriptor
