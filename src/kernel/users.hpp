#pragma once

#include <stddef.h>
#include <stdint.h>

namespace users {

struct User {
    char name[32];
    uint64_t allowed_caps;
    uint64_t generation;
    uint32_t password_iterations;
    uint8_t password_salt[16];
    uint8_t password_hash[32];
    bool password_set;
    bool active;
};

constexpr size_t kMaxUsers = 32;

void init();
User* create(const char* name, uint64_t allowed_caps);
User* find(const char* name);
void bump_generation(User& user);
bool allows(const User& user, uint64_t cap_bitmask);
bool set_password(User& user,
                  const uint8_t* salt,
                  const uint8_t* hash,
                  uint32_t iterations);
void set_storage_path(const char* path);
bool load_from_disk();
bool persist();

}  // namespace users
