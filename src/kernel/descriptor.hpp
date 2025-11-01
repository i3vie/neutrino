#pragma once

#include <stddef.h>
#include <stdint.h>

namespace process {
struct Process;
}

namespace descriptor {

constexpr size_t kMaxDescriptors = 32;
constexpr uint32_t kTypeShift = 22;
constexpr uint32_t kTypeMask = 0x3FF;
constexpr uint32_t kCapabilityMask = (1u << kTypeShift) - 1u;

constexpr uint32_t kTypeConsole = 0x001;
constexpr uint32_t kTypeSerial  = 0x002;
constexpr uint32_t kTypeKeyboard = 0x003;

enum Capability : uint32_t {
    CapabilityReadable = 1u << 0,
    CapabilityWritable = 1u << 1,
    CapabilitySeekable = 1u << 2,
    CapabilityMappable = 1u << 3,
};

struct Ops;

struct Entry {
    uint32_t info;
    void* object;
    void (*close)(void*);
    const Ops* ops;
    bool in_use;
};

struct Table {
    Entry entries[kMaxDescriptors];
};

struct Ops {
    int64_t (*read)(process::Process& proc,
                    Entry& entry,
                    uint64_t user_address,
                    uint64_t length,
                    uint64_t offset);
    int64_t (*write)(process::Process& proc,
                     Entry& entry,
                     uint64_t user_address,
                     uint64_t length,
                     uint64_t offset);
};

struct Allocation {
    uint32_t info;
    void* object;
    void (*close)(void*);
    const Ops* ops;
};

using OpenFn = bool (*)(process::Process& proc,
                        uint64_t arg0,
                        uint64_t arg1,
                        uint64_t arg2,
                        Allocation& out_allocation);

bool register_type(uint32_t type, OpenFn open, const Ops* ops);

void init();
void register_builtin_types();

bool transfer_console_owner(process::Process& from, process::Process& to);
void restore_console_owner(process::Process& proc);

void init_table(Table& table);
void destroy_table(process::Process& proc, Table& table);

int32_t install(process::Process& proc, Table& table, const Allocation& alloc);
int32_t open(process::Process& proc,
             Table& table,
             uint32_t type,
             uint64_t arg0,
             uint64_t arg1,
             uint64_t arg2);
uint32_t query(const Table& table, uint32_t handle, bool* ok);
int64_t read(process::Process& proc,
             Table& table,
             uint32_t handle,
             uint64_t user_address,
             uint64_t length,
             uint64_t offset);
int64_t write(process::Process& proc,
              Table& table,
              uint32_t handle,
              uint64_t user_address,
              uint64_t length,
              uint64_t offset);
bool close(process::Process& proc, Table& table, uint32_t handle);

constexpr uint32_t make_info(uint32_t type, uint32_t capabilities) {
    return ((type & kTypeMask) << kTypeShift) |
           (capabilities & kCapabilityMask);
}

constexpr uint32_t info_type(uint32_t info) {
    return (info >> kTypeShift) & kTypeMask;
}

constexpr uint32_t info_capabilities(uint32_t info) {
    return info & kCapabilityMask;
}

inline bool has_capability(uint32_t info, Capability capability) {
    return (info_capabilities(info) & static_cast<uint32_t>(capability)) != 0;
}

}  // namespace descriptor
