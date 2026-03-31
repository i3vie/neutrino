#pragma once

#include <stddef.h>
#include <stdint.h>

namespace users {

struct User {
    char name[32];
    uint64_t allowed_caps;
    uint64_t generation;
    bool active;
};

constexpr size_t kMaxUsers = 32;

void init();
User* create(const char* name, uint64_t allowed_caps);
User* find(const char* name);
void bump_generation(User& user);
bool allows(const User& user, uint64_t cap_bitmask);
void set_storage_path(const char* path);
bool load_from_disk();
bool persist();

}  // namespace users
