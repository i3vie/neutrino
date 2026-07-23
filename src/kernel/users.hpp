#pragma once

#include <stddef.h>
#include <stdint.h>

namespace users {

struct UserId {
    uint64_t machine;
    uint64_t local;
};

struct User {
    UserId id;
    char name[32];
    uint64_t allowed_caps;
    uint64_t generation;
    uint32_t password_iterations;
    uint8_t password_salt[16];
    uint8_t password_hash[32];
    bool password_set;
    bool active;
};

struct UserInfo {
    uint64_t id_machine;
    uint64_t id_local;
    char name[32];
    uint64_t allowed_caps;
    uint64_t generation;
    uint32_t password_set;
    uint32_t active;
};

constexpr size_t kMaxUsers = 32;

void init();
User* create(const char* name, uint64_t allowed_caps);
User* find(const char* name);
User* find(const UserId& id);
uint64_t handle_for(const User* user);
User* from_handle(uint64_t handle);
UserId machine_id();
void bump_generation(User& user);
bool allows(const User& user, uint64_t cap_bitmask);
// Atomic snapshots used by capability revocation checks. These helpers keep
// backing-user state lock-free so the capability lock is the only lock in the
// principal/token hierarchy.
bool active_generation(const User& user, uint64_t& out_generation);
bool generation_allows(const User& user,
                       uint64_t expected_generation,
                       uint64_t cap_bitmask);
bool snapshot_info(const User& user, UserInfo& out_info);
bool set_password(User& user,
                  const uint8_t* salt,
                  const uint8_t* hash,
                  uint32_t iterations);
void set_storage_path(const char* path);
bool load_from_disk();
bool persist();

}  // namespace users
