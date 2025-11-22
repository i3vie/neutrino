#pragma once

#include <stddef.h>
#include <stdint.h>

#include "../include/descriptors.hpp"

namespace process {
struct Process;
}

struct Framebuffer;

namespace fs {
struct BlockDevice;
}  // namespace fs

namespace descriptor {

constexpr size_t kMaxDescriptors = 640;
constexpr uint32_t kInvalidHandle = 0xFFFFFFFFu;

constexpr uint32_t kHandleIndexBits = 16;
constexpr uint32_t kHandleIndexMask = (1u << kHandleIndexBits) - 1u;
constexpr uint32_t kHandleGenerationShift = kHandleIndexBits;

constexpr uint32_t kTypeConsole =
    static_cast<uint32_t>(descriptor_defs::Type::Console);
constexpr uint32_t kTypeSerial =
    static_cast<uint32_t>(descriptor_defs::Type::Serial);
constexpr uint32_t kTypeKeyboard =
    static_cast<uint32_t>(descriptor_defs::Type::Keyboard);
constexpr uint32_t kTypeFramebuffer =
    static_cast<uint32_t>(descriptor_defs::Type::Framebuffer);
constexpr uint32_t kTypeBlockDevice =
    static_cast<uint32_t>(descriptor_defs::Type::BlockDevice);
constexpr uint32_t kTypePipe =
    static_cast<uint32_t>(descriptor_defs::Type::Pipe);

constexpr int64_t kWouldBlock = -2;

enum class Flag : uint64_t {
    Readable   = 1ull << 0,
    Writable   = 1ull << 1,
    Seekable   = 1ull << 2,
    Mappable   = 1ull << 3,
    Async      = 1ull << 8,
    EventSource = 1ull << 9,
    Device     = 1ull << 10,
    Block      = 1ull << 11,
};

struct DescriptorEntry;

struct DescriptorExt {
    uint64_t flags;
    void* data;
    DescriptorExt* next;
};

struct Ops {
    int64_t (*read)(process::Process& proc,
                    DescriptorEntry& entry,
                    uint64_t user_address,
                    uint64_t length,
                    uint64_t offset);
    int64_t (*write)(process::Process& proc,
                     DescriptorEntry& entry,
                     uint64_t user_address,
                     uint64_t length,
                     uint64_t offset);
    int (*get_property)(DescriptorEntry& entry,
                        uint32_t property,
                        void* out,
                        size_t size);
    int (*set_property)(DescriptorEntry& entry,
                        uint32_t property,
                        const void* in,
                        size_t size);
};

struct DescriptorEntry {
    uint16_t type;
    uint16_t generation;
    uint32_t refcount;
    uint64_t flags;
    uint64_t extended_flags;
    uint64_t created_tick;
    uint64_t last_access_tick;
    void* object;
    void* subsystem_data;
    const char* name;
    const Ops* ops;
    DescriptorExt* ext;
    void (*close)(DescriptorEntry& entry);
    uint64_t lock_word;
    bool has_extended_flags;
    bool in_use;
};

struct Table {
    DescriptorEntry entries[kMaxDescriptors];
};

struct Allocation {
    uint16_t type;
    uint64_t flags;
    uint64_t extended_flags;
    bool has_extended_flags;
    void* object;
    void* subsystem_data;
    const char* name;
    const Ops* ops;
    DescriptorExt* ext;
    void (*close)(DescriptorEntry& entry);
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

uint32_t install(process::Process& proc,
                 Table& table,
                 const Allocation& alloc);
uint32_t open(process::Process& proc,
              Table& table,
              uint32_t type,
              uint64_t arg0,
              uint64_t arg1,
              uint64_t arg2);
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

bool get_type(const Table& table, uint32_t handle, uint16_t& out_type);
bool test_flag(const Table& table,
               uint32_t handle,
               uint64_t flag,
               bool& out_value);
bool get_flags(const Table& table,
               uint32_t handle,
               bool extended_set,
               uint64_t& out_flags);
int get_property(process::Process& proc,
                 Table& table,
                 uint32_t handle,
                 uint32_t property,
                 uint64_t out_ptr,
                 uint64_t size);
int set_property(process::Process& proc,
                 Table& table,
                 uint32_t handle,
                 uint32_t property,
                 uint64_t in_ptr,
                 uint64_t size);

void register_framebuffer_device(Framebuffer& framebuffer,
                                 uint64_t physical_base);
bool register_block_device(fs::BlockDevice& device, bool lock_for_kernel);
void reset_block_device_registry();

uint32_t open_kernel(uint32_t type,
                     uint64_t arg0,
                     uint64_t arg1,
                     uint64_t arg2);
int64_t read_kernel(uint32_t handle,
                    void* buffer,
                    uint64_t length,
                    uint64_t offset);
int64_t write_kernel(uint32_t handle,
                     const void* buffer,
                     uint64_t length,
                     uint64_t offset);
bool close_kernel(uint32_t handle);
int get_property_kernel(uint32_t handle,
                        uint32_t property,
                        void* out,
                        uint64_t size);
int set_property_kernel(uint32_t handle,
                        uint32_t property,
                        const void* in,
                        uint64_t size);
bool is_kernel_process(const process::Process& proc);

inline constexpr uint16_t handle_index(uint32_t handle) {
    return static_cast<uint16_t>(handle & kHandleIndexMask);
}

inline constexpr uint16_t handle_generation(uint32_t handle) {
    return static_cast<uint16_t>(handle >> kHandleGenerationShift);
}

inline constexpr uint32_t make_handle(uint16_t index, uint16_t generation) {
    return (static_cast<uint32_t>(generation) << kHandleGenerationShift) |
           static_cast<uint32_t>(index);
}

inline bool has_flag(uint64_t flags, Flag flag) {
    return (flags & static_cast<uint64_t>(flag)) != 0;
}

}  // namespace descriptor
