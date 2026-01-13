#include "descriptor.hpp"

#include "process.hpp"
#include "arch/x86_64/memory/paging.hpp"
#include "string_util.hpp"
#include "../lib/mem.hpp"

namespace descriptor {

namespace {

process::Process g_kernel_process{};
bool g_kernel_process_initialized = false;

process::Process& kernel_process() {
    if (!g_kernel_process_initialized) {
        memset(&g_kernel_process, 0, sizeof(g_kernel_process));
        init_table(g_kernel_process.descriptors);
        g_kernel_process.cr3 = paging_kernel_cr3();
        g_kernel_process.fs_base = 0;
        g_kernel_process_initialized = true;
    }
    if (g_kernel_process.cr3 == 0) {
        g_kernel_process.cr3 = paging_kernel_cr3();
    }
    return g_kernel_process;
}

struct TypeRegistration {
    uint16_t type;
    OpenFn open;
    const Ops* ops;
    bool used;
};

constexpr size_t kMaxRegisteredTypes = 32;
TypeRegistration g_type_registry[kMaxRegisteredTypes];

TypeRegistration* find_registration(uint16_t type) {
    for (size_t i = 0; i < kMaxRegisteredTypes; ++i) {
        if (g_type_registry[i].used && g_type_registry[i].type == type) {
            return &g_type_registry[i];
        }
    }
    return nullptr;
}

void reset_entry(DescriptorEntry& entry, bool bump_generation) {
    uint16_t generation = entry.generation;
    if (bump_generation) {
        uint16_t next_generation =
            static_cast<uint16_t>(static_cast<uint32_t>(generation) + 1u);
        generation = (next_generation == 0) ? 1 : next_generation;
    }
    entry.type = 0;
    entry.flags = 0;
    entry.extended_flags = 0;
    entry.created_tick = 0;
    entry.last_access_tick = 0;
    entry.object = nullptr;
    entry.subsystem_data = nullptr;
    entry.name = nullptr;
    entry.ops = nullptr;
    entry.ext = nullptr;
    entry.close = nullptr;
    entry.lock_word = 0;
    entry.refcount = 0;
    entry.has_extended_flags = false;
    entry.in_use = false;
    entry.generation = (generation == 0) ? 1 : generation;
}

DescriptorEntry* lookup_entry(Table& table, uint32_t handle) {
    uint16_t index = handle_index(handle);
    uint16_t generation = handle_generation(handle);
    if (index >= kMaxDescriptors) {
        return nullptr;
    }
    DescriptorEntry& entry = table.entries[index];
    if (!entry.in_use) {
        return nullptr;
    }
    if (generation == 0 || entry.generation != generation) {
        return nullptr;
    }
    return &entry;
}

const DescriptorEntry* lookup_entry(const Table& table, uint32_t handle) {
    uint16_t index = handle_index(handle);
    uint16_t generation = handle_generation(handle);
    if (index >= kMaxDescriptors) {
        return nullptr;
    }
    const DescriptorEntry& entry = table.entries[index];
    if (!entry.in_use) {
        return nullptr;
    }
    if (generation == 0 || entry.generation != generation) {
        return nullptr;
    }
    return &entry;
}

}  // namespace

bool register_type(uint32_t type, OpenFn open, const Ops* ops) {
    if (type > 0xFFFFu) {
        return false;
    }
    uint16_t type_id = static_cast<uint16_t>(type);
    if (find_registration(type_id) != nullptr) {
        return false;
    }
    for (size_t i = 0; i < kMaxRegisteredTypes; ++i) {
        if (!g_type_registry[i].used) {
            g_type_registry[i] = TypeRegistration{
                .type = type_id,
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
    g_kernel_process_initialized = false;
    (void)kernel_process();
}

void init_table(Table& table) {
    for (size_t i = 0; i < kMaxDescriptors; ++i) {
        table.entries[i].generation = 1;
        reset_entry(table.entries[i], false);
    }
}

void destroy_table(process::Process& proc, Table& table) {
    (void)proc;
    for (size_t i = 0; i < kMaxDescriptors; ++i) {
        DescriptorEntry& entry = table.entries[i];
        if (!entry.in_use) {
            reset_entry(entry, false);
            continue;
        }
        if (entry.close != nullptr) {
            entry.close(entry);
        }
        reset_entry(entry, true);
    }
}

uint32_t install(process::Process& proc,
                 Table& table,
                 const Allocation& alloc) {
    (void)proc;
    for (size_t i = 0; i < kMaxDescriptors; ++i) {
        DescriptorEntry& entry = table.entries[i];
        if (entry.in_use) {
            continue;
        }
        uint16_t generation = entry.generation;
        if (generation == 0) {
            generation = 1;
        }
        reset_entry(entry, false);
        entry.type = alloc.type;
        entry.flags = alloc.flags;
        entry.extended_flags = alloc.extended_flags;
        entry.has_extended_flags = alloc.has_extended_flags;
        entry.object = alloc.object;
        entry.subsystem_data = alloc.subsystem_data;
        entry.name = alloc.name;
        entry.ops = alloc.ops;
        entry.ext = alloc.ext;
        entry.close = alloc.close;
        entry.refcount = 1;
        entry.created_tick = 0;
        entry.last_access_tick = 0;
        entry.lock_word = 0;
        entry.in_use = true;
        entry.generation = generation;
        return make_handle(static_cast<uint16_t>(i), generation);
    }
    return kInvalidHandle;
}

uint32_t open(process::Process& proc,
              Table& table,
              uint32_t type,
              uint64_t arg0,
              uint64_t arg1,
              uint64_t arg2) {
    uint16_t type_id = static_cast<uint16_t>(type & 0xFFFFu);
    TypeRegistration* reg = find_registration(type_id);
    if (reg == nullptr || reg->open == nullptr) {
        return kInvalidHandle;
    }
    Allocation alloc{
        .type = type_id,
        .flags = 0,
        .extended_flags = 0,
        .has_extended_flags = false,
        .object = nullptr,
        .subsystem_data = nullptr,
        .name = nullptr,
        .ops = reg->ops,
        .ext = nullptr,
        .close = nullptr,
    };
    if (!reg->open(proc, arg0, arg1, arg2, alloc)) {
        if (alloc.close != nullptr) {
            DescriptorEntry temp{};
            temp.type = alloc.type;
            temp.flags = alloc.flags;
            temp.extended_flags = alloc.extended_flags;
            temp.has_extended_flags = alloc.has_extended_flags;
            temp.object = alloc.object;
            temp.subsystem_data = alloc.subsystem_data;
            temp.name = alloc.name;
            temp.ops = alloc.ops;
            temp.close = alloc.close;
            temp.in_use = true;
            alloc.close(temp);
        }
        return kInvalidHandle;
    }
    alloc.type = (alloc.type == 0) ? type_id : alloc.type;
    if (alloc.ops == nullptr) {
        alloc.ops = reg->ops;
    }
    uint32_t handle = install(proc, table, alloc);
    if (handle == kInvalidHandle && alloc.close != nullptr) {
        DescriptorEntry temp{};
        temp.type = alloc.type;
        temp.flags = alloc.flags;
        temp.extended_flags = alloc.extended_flags;
        temp.has_extended_flags = alloc.has_extended_flags;
        temp.object = alloc.object;
        temp.subsystem_data = alloc.subsystem_data;
        temp.name = alloc.name;
        temp.ops = alloc.ops;
        temp.close = alloc.close;
        temp.in_use = true;
        alloc.close(temp);
    }
    return handle;
}

int64_t read(process::Process& proc,
             Table& table,
             uint32_t handle,
             uint64_t user_address,
             uint64_t length,
             uint64_t offset) {
    DescriptorEntry* entry = lookup_entry(table, handle);
    if (entry == nullptr) {
        return -1;
    }
    if (!has_flag(entry->flags, Flag::Readable)) {
        return -1;
    }
    if (entry->ops == nullptr || entry->ops->read == nullptr) {
        return -1;
    }
    entry->last_access_tick = 0;
    return entry->ops->read(proc, *entry, user_address, length, offset);
}

int64_t write(process::Process& proc,
              Table& table,
              uint32_t handle,
              uint64_t user_address,
              uint64_t length,
              uint64_t offset) {
    DescriptorEntry* entry = lookup_entry(table, handle);
    if (entry == nullptr) {
        return -1;
    }
    if (!has_flag(entry->flags, Flag::Writable)) {
        return -1;
    }
    if (entry->ops == nullptr || entry->ops->write == nullptr) {
        return -1;
    }
    entry->last_access_tick = 0;
    return entry->ops->write(proc, *entry, user_address, length, offset);
}

bool close(process::Process& proc, Table& table, uint32_t handle) {
    (void)proc;
    DescriptorEntry* entry = lookup_entry(table, handle);
    if (entry == nullptr) {
        return false;
    }
    if (entry->close != nullptr) {
        entry->close(*entry);
    }
    reset_entry(*entry, true);
    return true;
}

bool get_type(const Table& table, uint32_t handle, uint16_t& out_type) {
    const DescriptorEntry* entry = lookup_entry(table, handle);
    if (entry == nullptr) {
        return false;
    }
    out_type = entry->type;
    return true;
}

bool test_flag(const Table& table,
               uint32_t handle,
               uint64_t flag,
               bool& out_value) {
    const DescriptorEntry* entry = lookup_entry(table, handle);
    if (entry == nullptr) {
        return false;
    }
    uint64_t source = entry->flags;
    if ((source & flag) == 0 && entry->has_extended_flags) {
        source = entry->extended_flags;
    }
    out_value = (source & flag) != 0;
    return true;
}

bool get_flags(const Table& table,
               uint32_t handle,
               bool extended_set,
               uint64_t& out_flags) {
    const DescriptorEntry* entry = lookup_entry(table, handle);
    if (entry == nullptr) {
        return false;
    }
    out_flags = extended_set
                    ? (entry->has_extended_flags ? entry->extended_flags : 0)
                    : entry->flags;
    return true;
}

int get_property(process::Process& proc,
                 Table& table,
                 uint32_t handle,
                 uint32_t property,
                 uint64_t out_ptr,
                 uint64_t size) {
    (void)proc;
    DescriptorEntry* entry = lookup_entry(table, handle);
    if (entry == nullptr) {
        return -1;
    }
    void* out = reinterpret_cast<void*>(out_ptr);
    size_t out_size = static_cast<size_t>(size);
    if (property ==
        static_cast<uint32_t>(descriptor_defs::Property::CommonName)) {
        if (entry->name == nullptr || out == nullptr || out_size == 0) {
            return -1;
        }
        size_t name_len = string_util::length(entry->name);
        if (name_len + 1 > out_size) {
            return -1;
        }
        char* dest = reinterpret_cast<char*>(out);
        for (size_t i = 0; i <= name_len; ++i) {
            dest[i] = entry->name[i];
        }
        return 0;
    }
    if (entry->ops == nullptr || entry->ops->get_property == nullptr) {
        return -1;
    }
    if (out == nullptr && out_size != 0) {
        return -1;
    }
    return entry->ops->get_property(*entry, property, out, out_size);
}

int set_property(process::Process& proc,
                 Table& table,
                 uint32_t handle,
                 uint32_t property,
                 uint64_t in_ptr,
                 uint64_t size) {
    (void)proc;
    DescriptorEntry* entry = lookup_entry(table, handle);
    if (entry == nullptr) {
        return -1;
    }
    if (entry->ops == nullptr || entry->ops->set_property == nullptr) {
        return -1;
    }
    const void* in = reinterpret_cast<const void*>(in_ptr);
    if (in == nullptr && size != 0) {
        return -1;
    }
    return entry->ops->set_property(*entry, property, in, static_cast<size_t>(size));
}

uint32_t open_kernel(uint32_t type,
                     uint64_t arg0,
                     uint64_t arg1,
                     uint64_t arg2) {
    process::Process& proc = kernel_process();
    return open(proc, proc.descriptors, type, arg0, arg1, arg2);
}

int64_t read_kernel(uint32_t handle,
                    void* buffer,
                    uint64_t length,
                    uint64_t offset) {
    process::Process& proc = kernel_process();
    return read(proc,
                proc.descriptors,
                handle,
                reinterpret_cast<uint64_t>(buffer),
                length,
                offset);
}

int64_t write_kernel(uint32_t handle,
                     const void* buffer,
                     uint64_t length,
                     uint64_t offset) {
    process::Process& proc = kernel_process();
    return write(proc,
                 proc.descriptors,
                 handle,
                 reinterpret_cast<uint64_t>(buffer),
                 length,
                 offset);
}

bool close_kernel(uint32_t handle) {
    process::Process& proc = kernel_process();
    return close(proc, proc.descriptors, handle);
}

int get_property_kernel(uint32_t handle,
                        uint32_t property,
                        void* out,
                        uint64_t size) {
    process::Process& proc = kernel_process();
    return get_property(proc,
                        proc.descriptors,
                        handle,
                        property,
                        reinterpret_cast<uint64_t>(out),
                        size);
}

int set_property_kernel(uint32_t handle,
                        uint32_t property,
                        const void* in,
                        uint64_t size) {
    process::Process& proc = kernel_process();
    return set_property(proc,
                        proc.descriptors,
                        handle,
                        property,
                        reinterpret_cast<uint64_t>(in),
                        size);
}

bool is_kernel_process(const process::Process& proc) {
    return &proc == &kernel_process();
}

}  // namespace descriptor
