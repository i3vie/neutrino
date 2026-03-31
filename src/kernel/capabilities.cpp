#include "capabilities.hpp"

#include <stddef.h>
#include <stdint.h>

#include "../lib/mem.hpp"
#include "users.hpp"

namespace {

capabilities::Principal g_principals[capabilities::kMaxPrincipals];
capabilities::CapabilityToken g_tokens[capabilities::kMaxCapabilityTokens];

uint64_t g_handle_nonce = 0xC001D00Du;

uint64_t next_handle() {
    // Very small PRNG: xorshift64*
    uint64_t x = g_handle_nonce;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    g_handle_nonce = x;
    return x * 0x2545F4914F6CDD1Dull;
}

}  // namespace

namespace capabilities {

void init() {
    memset(g_principals, 0, sizeof(g_principals));
    memset(g_tokens, 0, sizeof(g_tokens));
    g_handle_nonce = 0xC001D00Du;
}

Principal* create_principal(void* backing_user, CapabilityMask allowed_caps) {
    for (size_t i = 0; i < kMaxPrincipals; ++i) {
        Principal& p = g_principals[i];
        if (!p.active) {
            p.backing_user = backing_user;
            p.allowed_caps = allowed_caps;
            p.generation = 1;
            p.refcount = 1;
            p.active = true;
            return &p;
        }
    }
    return nullptr;
}

void principal_add_ref(Principal* principal) {
    if (principal == nullptr || !principal->active) {
        return;
    }
    ++principal->refcount;
}

void principal_release(Principal* principal) {
    if (principal == nullptr || !principal->active) {
        return;
    }
    if (principal->refcount > 0) {
        --principal->refcount;
    }
    if (principal->refcount == 0) {
        principal->active = false;
        principal->allowed_caps = 0;
        principal->generation = 0;
        principal->backing_user = nullptr;
    }
}

void principal_bump_generation(Principal& principal) {
    if (!principal.active) {
        return;
    }
    ++principal.generation;
}

bool principal_allows(const Principal& principal, CapabilityKind kind) {
    if (!principal.active) {
        return false;
    }
    uint64_t bit = capability_bit(kind);
    if ((principal.allowed_caps & bit) == 0) {
        return false;
    }
    if (principal.backing_user != nullptr) {
        auto* user = reinterpret_cast<users::User*>(principal.backing_user);
        if (!users::allows(*user, bit)) {
            return false;
        }
    }
    return true;
}

bool principal_is_valid(const Principal* principal) {
    return principal != nullptr && principal->active;
}

Principal* principal_from_handle(uint64_t handle) {
    if (handle == 0) {
        return nullptr;
    }
    size_t idx = static_cast<size_t>(handle % kMaxPrincipals);
    if (!g_principals[idx].active) {
        return nullptr;
    }
    return &g_principals[idx];
}

bool capability_from_value(uint64_t value, CapabilityKind& out_kind) {
    if (value >= static_cast<uint64_t>(CapabilityKind::Count)) {
        return false;
    }
    out_kind = static_cast<CapabilityKind>(value);
    return true;
}

CapabilityToken* issue_token(Principal& issuer, CapabilityKind kind) {
    if (!issuer.active || !principal_allows(issuer, kind)) {
        return nullptr;
    }
    for (size_t i = 0; i < kMaxCapabilityTokens; ++i) {
        CapabilityToken& tok = g_tokens[i];
        if (tok.issuer == nullptr) {
            tok.issuer = &issuer;
            tok.kind = kind;
            tok.generation_snapshot = issuer.generation;
            return &tok;
        }
    }
    return nullptr;
}

bool token_valid(const CapabilityToken& token) {
    if (token.issuer == nullptr) {
        return false;
    }
    if (!token.issuer->active) {
        return false;
    }
    return token.generation_snapshot == token.issuer->generation;
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

bool cap_table_insert(CapHandleEntry* table,
                      size_t capacity,
                      CapabilityToken* token,
                      uint64_t& out_handle) {
    if (table == nullptr || token == nullptr || capacity == 0) {
        return false;
    }
    for (size_t attempts = 0; attempts < capacity; ++attempts) {
        uint64_t handle = next_handle();
        size_t slot = cap_table_slot(table, capacity, handle);
        if (slot >= capacity) {
            continue;
        }
        if (!table[slot].in_use) {
            table[slot].in_use = true;
            table[slot].handle = handle;
            table[slot].token = token;
            out_handle = handle;
            return true;
        }
    }
    return false;
}

CapabilityToken* cap_table_lookup(CapHandleEntry* table,
                                  size_t capacity,
                                  uint64_t handle,
                                  bool invalidate_if_stale) {
    if (table == nullptr || capacity == 0) {
        return nullptr;
    }
    size_t idx = static_cast<size_t>(handle ^ (handle >> 32)) % capacity;
    for (size_t probe = 0; probe < capacity; ++probe) {
        size_t slot = (idx + probe) % capacity;
        CapHandleEntry& entry = table[slot];
        if (!entry.in_use) {
            return nullptr;
        }
        if (entry.handle == handle) {
            CapabilityToken* tok = entry.token;
            if (tok == nullptr || !token_valid(*tok)) {
                if (invalidate_if_stale) {
                    entry.in_use = false;
                    entry.token = nullptr;
                    entry.handle = 0;
                }
                return nullptr;
            }
            return tok;
        }
    }
    return nullptr;
}

void cap_table_clear(CapHandleEntry* table, size_t capacity) {
    if (table == nullptr) {
        return;
    }
    for (size_t i = 0; i < capacity; ++i) {
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
    for (size_t i = 0; i < handle_count; ++i) {
        uint64_t h = handles[i];
        CapabilityToken* tok = cap_table_lookup(src, src_capacity, h, false);
        if (tok == nullptr) {
            return false;
        }
        uint64_t new_handle = 0;
        if (!cap_table_insert(dest, dest_capacity, tok, new_handle)) {
            return false;
        }
    }
    return true;
}

}  // namespace capabilities
