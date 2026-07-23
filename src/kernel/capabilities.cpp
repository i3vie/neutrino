#include "capabilities.hpp"

#include <stddef.h>
#include <stdint.h>

#include "../lib/mem.hpp"
#include "sync.hpp"
#include "users.hpp"

namespace {

capabilities::Principal g_principals[capabilities::kMaxPrincipals];
capabilities::CapabilityToken g_tokens[capabilities::kMaxCapabilityTokens];

uint64_t g_handle_nonce = 0xC001D00Du;
sync::SpinLock g_capability_lock;

bool principal_index(const capabilities::Principal* principal,
                     size_t& out_index) {
    if (principal == nullptr) {
        return false;
    }
    uintptr_t address = reinterpret_cast<uintptr_t>(principal);
    uintptr_t begin = reinterpret_cast<uintptr_t>(&g_principals[0]);
    uintptr_t end = reinterpret_cast<uintptr_t>(
        &g_principals[capabilities::kMaxPrincipals]);
    if (address < begin || address >= end) {
        return false;
    }
    uintptr_t offset = address - begin;
    if ((offset % sizeof(capabilities::Principal)) != 0) {
        return false;
    }
    out_index = static_cast<size_t>(offset / sizeof(capabilities::Principal));
    return true;
}

uint64_t next_handle() {
    // Very small PRNG: xorshift64*
    uint64_t x = g_handle_nonce;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    g_handle_nonce = x;
    return x * 0x2545F4914F6CDD1Dull;
}

void release_token_reference(capabilities::CapabilityToken* token) {
    if (token == nullptr || token->issuer == nullptr) {
        return;
    }
    if (token->refcount > 0) {
        --token->refcount;
    }
    if (token->refcount == 0) {
        token->issuer = nullptr;
        token->generation_snapshot = 0;
        token->refcount = 0;
    }
}

bool principal_is_valid_locked(const capabilities::Principal* principal) {
    size_t index = 0;
    if (!principal_index(principal, index)) {
        return false;
    }
    const capabilities::Principal& stored = g_principals[index];
    if (!stored.active) {
        return false;
    }
    if (stored.backing_user == nullptr) {
        return true;
    }
    uint64_t generation = 0;
    return users::active_generation(
               *reinterpret_cast<const users::User*>(stored.backing_user),
               generation) &&
           generation == stored.backing_generation_snapshot;
}

bool principal_allows_locked(const capabilities::Principal& principal,
                             capabilities::CapabilityKind kind) {
    if (!principal_is_valid_locked(&principal)) {
        return false;
    }
    uint64_t bit = capabilities::capability_bit(kind);
    if (!capabilities::mask_allows(principal.allowed_caps, bit)) {
        return false;
    }
    if (principal.backing_user == nullptr) {
        return true;
    }
    return users::generation_allows(
        *reinterpret_cast<const users::User*>(principal.backing_user),
        principal.backing_generation_snapshot,
        bit);
}

bool token_valid_locked(const capabilities::CapabilityToken& token) {
    if (token.issuer == nullptr ||
        !principal_is_valid_locked(token.issuer)) {
        return false;
    }
    return token.generation_snapshot == token.issuer->generation &&
           principal_allows_locked(*token.issuer, token.kind);
}

capabilities::Principal* principal_from_handle_locked(uint64_t handle) {
    uint32_t encoded_index = static_cast<uint32_t>(handle);
    uint64_t generation = handle >> 32;
    if (encoded_index == 0 || generation == 0) {
        return nullptr;
    }
    size_t idx = static_cast<size_t>(encoded_index - 1u);
    if (idx >= capabilities::kMaxPrincipals ||
        !g_principals[idx].active ||
        g_principals[idx].generation != generation) {
        return nullptr;
    }
    return &g_principals[idx];
}

}  // namespace

namespace capabilities {

void init() {
    sync::IrqLockGuard guard(g_capability_lock);
    memset(g_principals, 0, sizeof(g_principals));
    memset(g_tokens, 0, sizeof(g_tokens));
    g_handle_nonce = 0xC001D00Du;
}

Principal* create_principal(void* backing_user, CapabilityMask allowed_caps) {
    allowed_caps = normalize_mask(allowed_caps);
    uint64_t backing_generation = 0;
    if (backing_user != nullptr &&
        !users::active_generation(
            *reinterpret_cast<const users::User*>(backing_user),
            backing_generation)) {
        return nullptr;
    }
    sync::IrqLockGuard guard(g_capability_lock);
    for (size_t i = 0; i < kMaxPrincipals; ++i) {
        Principal& p = g_principals[i];
        if (!p.active) {
            p.backing_user = backing_user;
            p.backing_generation_snapshot = backing_generation;
            p.allowed_caps = allowed_caps;
            if (p.generation == 0) {
                p.generation = 1;
            }
            p.refcount = 1;
            p.active = true;
            return &p;
        }
    }
    return nullptr;
}

bool principal_add_ref(Principal* principal) {
    sync::IrqLockGuard guard(g_capability_lock);
    if (principal == nullptr) {
        return true;
    }
    if (!principal_is_valid_locked(principal) ||
        principal->refcount == UINT32_MAX) {
        return false;
    }
    ++principal->refcount;
    return true;
}

void principal_release(Principal* principal) {
    sync::IrqLockGuard guard(g_capability_lock);
    size_t index = 0;
    if (!principal_index(principal, index) || !g_principals[index].active) {
        return;
    }
    principal = &g_principals[index];
    if (principal->refcount > 0) {
        --principal->refcount;
    }
    if (principal->refcount == 0) {
        principal->active = false;
        principal->allowed_caps = 0;
        ++principal->generation;
        if (principal->generation == 0) {
            principal->generation = 1;
        }
        principal->backing_user = nullptr;
        principal->backing_generation_snapshot = 0;
    }
}

void principal_bump_generation(Principal& principal) {
    sync::IrqLockGuard guard(g_capability_lock);
    size_t index = 0;
    if (!principal_index(&principal, index) || !g_principals[index].active) {
        return;
    }
    Principal& stored = g_principals[index];
    ++stored.generation;
    if (stored.generation == 0) {
        stored.generation = 1;
    }
}

bool principal_allows(const Principal& principal, CapabilityKind kind) {
    sync::IrqLockGuard guard(g_capability_lock);
    return principal_allows_locked(principal, kind);
}

bool principal_is_valid(const Principal* principal) {
    sync::IrqLockGuard guard(g_capability_lock);
    return principal_is_valid_locked(principal);
}

bool principal_user_id(const Principal* principal,
                       uint64_t& out_machine_id,
                       uint64_t& out_local_id) {
    out_machine_id = 0;
    out_local_id = 0;
    sync::IrqLockGuard guard(g_capability_lock);
    if (!principal_is_valid_locked(principal) ||
        principal->backing_user == nullptr) {
        return false;
    }
    const auto* user =
        reinterpret_cast<const users::User*>(principal->backing_user);
    // User IDs are immutable for the lifetime of a pool entry. Validity and
    // generation were checked above while holding the principal lock.
    out_machine_id = user->id.machine;
    out_local_id = user->id.local;
    return out_local_id != 0;
}

Principal* principal_from_handle(uint64_t handle) {
    sync::IrqLockGuard guard(g_capability_lock);
    return principal_from_handle_locked(handle);
}

Principal* principal_acquire_from_handle(uint64_t handle) {
    sync::IrqLockGuard guard(g_capability_lock);
    Principal* principal = principal_from_handle_locked(handle);
    if (!principal_is_valid_locked(principal) ||
        principal->refcount == UINT32_MAX) {
        return nullptr;
    }
    ++principal->refcount;
    return principal;
}

uint64_t principal_handle(const Principal* principal) {
    sync::IrqLockGuard guard(g_capability_lock);
    size_t index = 0;
    if (!principal_index(principal, index) ||
        !principal_is_valid_locked(principal) || principal->generation == 0) {
        return 0;
    }
    return (principal->generation << 32) |
           (static_cast<uint64_t>(index) + 1u);
}

bool capability_from_value(uint64_t value, CapabilityKind& out_kind) {
    if (value >= static_cast<uint64_t>(CapabilityKind::Count)) {
        return false;
    }
    out_kind = static_cast<CapabilityKind>(value);
    return true;
}

CapabilityToken* issue_token(Principal& issuer, CapabilityKind kind) {
    sync::IrqLockGuard guard(g_capability_lock);
    if (!principal_allows_locked(issuer, kind)) {
        return nullptr;
    }
    for (size_t i = 0; i < kMaxCapabilityTokens; ++i) {
        CapabilityToken& tok = g_tokens[i];
        if (tok.issuer == nullptr) {
            tok.issuer = &issuer;
            tok.kind = kind;
            tok.generation_snapshot = issuer.generation;
            tok.refcount = 0;
            return &tok;
        }
    }
    return nullptr;
}

void discard_unreferenced_token(CapabilityToken* token) {
    if (token == nullptr) {
        return;
    }
    sync::IrqLockGuard guard(g_capability_lock);
    if (token->refcount == 0) {
        token->issuer = nullptr;
        token->generation_snapshot = 0;
    }
}

bool token_valid(const CapabilityToken& token) {
    sync::IrqLockGuard guard(g_capability_lock);
    return token_valid_locked(token);
}

static size_t cap_table_slot(CapHandleEntry* table,
                             size_t capacity,
                             uint64_t handle) {
    size_t idx = static_cast<size_t>(handle ^ (handle >> 32)) % capacity;
    for (size_t probe = 0; probe < capacity; ++probe) {
        size_t slot = (idx + probe) % capacity;
        CapHandleEntry& entry = table[slot];
        if (!entry.in_use || entry.handle == handle) {
            return slot;
        }
    }
    return capacity;  // full
}

static CapabilityToken* cap_table_lookup_locked(CapHandleEntry* table,
                                                size_t capacity,
                                                uint64_t handle,
                                                bool invalidate_if_stale) {
    size_t idx = static_cast<size_t>(handle ^ (handle >> 32)) % capacity;
    for (size_t probe = 0; probe < capacity; ++probe) {
        size_t slot = (idx + probe) % capacity;
        CapHandleEntry& entry = table[slot];
        if (!entry.in_use || entry.handle != handle) {
            continue;
        }
        CapabilityToken* tok = entry.token;
        if (tok == nullptr || !token_valid_locked(*tok)) {
            if (invalidate_if_stale) {
                release_token_reference(entry.token);
                entry.in_use = false;
                entry.token = nullptr;
                entry.handle = 0;
            }
            return nullptr;
        }
        return tok;
    }
    return nullptr;
}

static bool cap_table_insert_locked(CapHandleEntry* table,
                                    size_t capacity,
                                    CapabilityToken* token,
                                    uint64_t& out_handle) {
    if (!token_valid_locked(*token)) {
        return false;
    }
    for (size_t attempts = 0; attempts < capacity * 2; ++attempts) {
        uint64_t handle = next_handle();
        if (handle == 0) {
            continue;
        }
        size_t slot = cap_table_slot(table, capacity, handle);
        if (slot < capacity && !table[slot].in_use) {
            table[slot].in_use = true;
            table[slot].handle = handle;
            table[slot].token = token;
            ++token->refcount;
            out_handle = handle;
            return true;
        }
    }
    return false;
}

static void cap_table_remove_locked(CapHandleEntry* table,
                                    size_t capacity,
                                    uint64_t handle) {
    for (size_t i = 0; i < capacity; ++i) {
        if (!table[i].in_use || table[i].handle != handle) {
            continue;
        }
        release_token_reference(table[i].token);
        table[i].in_use = false;
        table[i].handle = 0;
        table[i].token = nullptr;
        return;
    }
}

bool cap_table_insert(CapHandleEntry* table,
                      size_t capacity,
                      CapabilityToken* token,
                      uint64_t& out_handle) {
    if (table == nullptr || token == nullptr || capacity == 0) {
        return false;
    }
    sync::IrqLockGuard guard(g_capability_lock);
    return cap_table_insert_locked(table, capacity, token, out_handle);
}

CapabilityToken* cap_table_lookup(CapHandleEntry* table,
                                  size_t capacity,
                                  uint64_t handle,
                                  bool invalidate_if_stale) {
    if (table == nullptr || capacity == 0) {
        return nullptr;
    }
    sync::IrqLockGuard guard(g_capability_lock);
    return cap_table_lookup_locked(table,
                                   capacity,
                                   handle,
                                   invalidate_if_stale);
}

void cap_table_clear(CapHandleEntry* table, size_t capacity) {
    if (table == nullptr) {
        return;
    }
    sync::IrqLockGuard guard(g_capability_lock);
    for (size_t i = 0; i < capacity; ++i) {
        if (table[i].in_use) {
            release_token_reference(table[i].token);
        }
        table[i].in_use = false;
        table[i].handle = 0;
        table[i].token = nullptr;
    }
}

bool cap_table_copy_handles(CapHandleEntry* dest,
                            size_t dest_capacity,
                            CapHandleEntry* src,
                            size_t src_capacity,
                            const uint64_t* handles,
                            size_t handle_count) {
    if (dest == nullptr || src == nullptr || handles == nullptr) {
        return false;
    }
    if (handle_count == 0) {
        return true;
    }
    if (handle_count > kMaxProcessCapabilities ||
        handle_count > dest_capacity) {
        return false;
    }
    sync::IrqLockGuard guard(g_capability_lock);
    size_t free_slots = 0;
    for (size_t i = 0; i < dest_capacity; ++i) {
        if (!dest[i].in_use) {
            ++free_slots;
        }
    }
    if (free_slots < handle_count) {
        return false;
    }
    for (size_t i = 0; i < handle_count; ++i) {
        if (cap_table_lookup_locked(src,
                                    src_capacity,
                                    handles[i],
                                    false) == nullptr) {
            return false;
        }
    }
    uint64_t inserted[kMaxProcessCapabilities]{};
    size_t inserted_count = 0;
    for (size_t i = 0; i < handle_count; ++i) {
        uint64_t h = handles[i];
        CapabilityToken* tok =
            cap_table_lookup_locked(src, src_capacity, h, false);
        uint64_t new_handle = 0;
        if (!cap_table_insert_locked(dest,
                                     dest_capacity,
                                     tok,
                                     new_handle)) {
            for (size_t rollback = 0; rollback < inserted_count; ++rollback) {
                cap_table_remove_locked(dest,
                                        dest_capacity,
                                        inserted[rollback]);
            }
            return false;
        }
        inserted[inserted_count++] = new_handle;
    }
    return true;
}

}  // namespace capabilities
