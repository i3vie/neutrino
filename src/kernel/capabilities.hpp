#pragma once

#include <stddef.h>
#include <stdint.h>

namespace capabilities {

using CapabilityMask = uint64_t;

// Compile-time capability kinds understood by the kernel.
enum class CapabilityKind : uint16_t {
    SysSettingsWrite = 0,
    BlockDeviceReadWrite = 1,
    ProcessSpawn = 2,
    Count,
};

struct Principal {
    void* backing_user;  // optional user pointer (users::User*), may be null
    uint64_t generation;
    CapabilityMask allowed_caps;
    uint32_t refcount;
    bool active;
};

struct CapabilityToken {
    Principal* issuer;
    CapabilityKind kind;
    uint64_t generation_snapshot;
};

// Per-process handle table entry. Handles are process-local opaque 64-bit ids.
struct CapHandleEntry {
    bool in_use;
    uint64_t handle;
    CapabilityToken* token;
};

constexpr size_t kMaxPrincipals = 64;
constexpr size_t kMaxCapabilityTokens = 256;
constexpr size_t kMaxProcessCapabilities = 32;

void init();

Principal* create_principal(void* backing_user, CapabilityMask allowed_caps);
void principal_add_ref(Principal* principal);
void principal_release(Principal* principal);
void principal_bump_generation(Principal& principal);
bool principal_allows(const Principal& principal, CapabilityKind kind);
bool principal_is_valid(const Principal* principal);
Principal* principal_from_handle(uint64_t handle);
bool capability_from_value(uint64_t value, CapabilityKind& out_kind);
inline uint64_t capability_bit(CapabilityKind kind) {
    return 1ull << static_cast<uint16_t>(kind);
}

CapabilityToken* issue_token(Principal& issuer, CapabilityKind kind);
bool token_valid(const CapabilityToken& token);

bool cap_table_insert(CapHandleEntry* table,
                      size_t capacity,
                      CapabilityToken* token,
                      uint64_t& out_handle);
CapabilityToken* cap_table_lookup(CapHandleEntry* table,
                                  size_t capacity,
                                  uint64_t handle,
                                  bool invalidate_if_stale);
void cap_table_clear(CapHandleEntry* table, size_t capacity);
bool cap_table_copy_handles(CapHandleEntry* dest,
                            size_t dest_capacity,
                            CapHandleEntry* src,
                            size_t src_capacity,
                            const uint64_t* handles,
                            size_t handle_count);

}  // namespace capabilities
