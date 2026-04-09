#include "users.hpp"

#include <stddef.h>
#include <stdint.h>

#include "string_util.hpp"
#include "../fs/vfs.hpp"
#include "../lib/mem.hpp"
#include "../drivers/log/logging.hpp"
#include "../kernel/path_util.hpp"

namespace {

users::User g_users[users::kMaxUsers];
char g_storage_path[128];
char g_storage_path_fallback[128];
bool g_has_storage_path = false;
bool g_loading_from_disk = false;

constexpr uint32_t kMagic = 0x4E544455;  // 'NTDU'
constexpr uint16_t kVersion = 1;

struct PackedUser {
    char name[32];
    uint64_t allowed_caps;
    uint64_t generation;
    uint8_t active;
    uint8_t reserved[7];
};

struct Header {
    uint32_t magic;
    uint16_t version;
    uint16_t entry_size;
    uint32_t count;
};

static bool path_valid() {
    return g_has_storage_path && g_storage_path[0] != '\0';
}

bool build_root_relative_path(const char* suffix,
                              char* out,
                              size_t out_size) {
    if (!path_valid() || suffix == nullptr || out == nullptr || out_size == 0) {
        return false;
    }
    size_t root_len = 0;
    while (g_storage_path[root_len] != '\0' && g_storage_path[root_len] != '/') {
        ++root_len;
    }
    if (root_len == 0 || root_len + 1 >= out_size) {
        return false;
    }
    for (size_t i = 0; i < root_len; ++i) {
        out[i] = g_storage_path[i];
    }
    size_t idx = root_len;
    out[idx++] = '/';
    for (size_t i = 0; suffix[i] != '\0'; ++i) {
        if (idx + 1 >= out_size) {
            return false;
        }
        out[idx++] = suffix[i];
    }
    out[idx] = '\0';
    return true;
}

void ensure_directory(const char* path) {
    if (path == nullptr || path[0] == '\0') {
        return;
    }
    vfs::DirectoryHandle handle{};
    if (vfs::open_directory(path, handle)) {
        vfs::close_directory(handle);
        return;
    }
    if (!vfs::create_directory(path)) {
        log_message(LogLevel::Warn, "Users: failed to create directory '%s'", path);
    }
}

void ensure_parent_directories(const char* path) {
    if (path == nullptr || path[0] == '\0') {
        return;
    }

    char current[path_util::kMaxPathLength];
    size_t path_len = string_util::length(path);
    if (path_len >= sizeof(current)) {
        path_len = sizeof(current) - 1;
    }
    for (size_t i = 0; i < path_len; ++i) {
        current[i] = path[i];
    }
    current[path_len] = '\0';

    int last_slash = -1;
    for (size_t i = 0; current[i] != '\0'; ++i) {
        if (current[i] == '/') {
            last_slash = static_cast<int>(i);
        }
    }
    if (last_slash <= 0) {
        return;
    }
    current[last_slash] = '\0';

    char partial[path_util::kMaxPathLength];
    size_t out = 0;
    for (size_t i = 0; current[i] != '\0' && out + 1 < sizeof(partial); ++i) {
        partial[out++] = current[i];
        partial[out] = '\0';
        if (current[i] == '/') {
            continue;
        }
        char next = current[i + 1];
        if (next == '/' || next == '\0') {
            ensure_directory(partial);
        }
    }
}

void ensure_home_directory(const char* name) {
    if (name == nullptr || name[0] == '\0') {
        return;
    }

    char user_root[128];
    if (!build_root_relative_path("user", user_root, sizeof(user_root))) {
        return;
    }
    ensure_directory(user_root);

    char user_home[128];
    size_t len = string_util::length(user_root);
    if (len + 1 >= sizeof(user_home)) {
        return;
    }
    string_util::copy(user_home, sizeof(user_home), user_root);
    user_home[len++] = '/';
    user_home[len] = '\0';
    string_util::copy(user_home + len, sizeof(user_home) - len, name);
    ensure_directory(user_home);
}

}  // namespace

namespace users {

void init() {
    for (size_t i = 0; i < kMaxUsers; ++i) {
        g_users[i].active = false;
        g_users[i].allowed_caps = 0;
        g_users[i].generation = 0;
        g_users[i].name[0] = '\0';
    }
}

User* create(const char* name, uint64_t allowed_caps) {
    if (name == nullptr) {
        return nullptr;
    }
    size_t len = string_util::length(name);
    if (len == 0 || len >= sizeof(g_users[0].name)) {
        return nullptr;
    }
    if (find(name) != nullptr) {
        return nullptr;  // already exists
    }
    for (size_t i = 0; i < kMaxUsers; ++i) {
        User& u = g_users[i];
        if (u.active) {
            continue;
        }
        string_util::copy(u.name, sizeof(u.name), name);
        u.allowed_caps = allowed_caps;
        u.generation = 1;
        u.active = true;
        User* out = &u;
        ensure_home_directory(u.name);
        if (!g_loading_from_disk) {
            if (!persist()) {
                log_message(LogLevel::Warn, "Users: persist failed after create");
            }
        }
        return out;
    }
    return nullptr;
}

User* find(const char* name) {
    if (name == nullptr) {
        return nullptr;
    }
    for (size_t i = 0; i < kMaxUsers; ++i) {
        User& u = g_users[i];
        if (!u.active) {
            continue;
        }
        if (string_util::equals(u.name, name)) {
            return &u;
        }
    }
    return nullptr;
}

void bump_generation(User& user) {
    if (!user.active) {
        return;
    }
    ++user.generation;
    // No persistence required; tokens are ephemeral across restarts.
}

bool allows(const User& user, uint64_t cap_bitmask) {
    if (!user.active) {
        return false;
    }
    return (user.allowed_caps & cap_bitmask) == cap_bitmask;
}

void set_storage_path(const char* path) {
    if (path == nullptr) {
        g_has_storage_path = false;
        g_storage_path[0] = '\0';
        g_storage_path_fallback[0] = '\0';
        return;
    }
    string_util::copy(g_storage_path, sizeof(g_storage_path), path);
    // Build fallback without the /system/ component.
    g_storage_path_fallback[0] = '\0';
    const char* system_pos = nullptr;
    for (size_t i = 0; g_storage_path[i] != '\0'; ++i) {
        const char* tail = g_storage_path + i;
        const char match[] = "/system/";
        size_t m = 0;
        while (match[m] != '\0' && tail[m] == match[m]) {
            ++m;
        }
        if (match[m] == '\0') {
            system_pos = g_storage_path + i;
            break;
        }
    }
    if (system_pos != nullptr) {
        size_t prefix = static_cast<size_t>(system_pos - g_storage_path);
        if (prefix + 1 < sizeof(g_storage_path_fallback)) {
            for (size_t i = 0; i < prefix; ++i) {
                g_storage_path_fallback[i] = g_storage_path[i];
            }
            size_t idx = prefix;
            g_storage_path_fallback[idx++] = '/';
            const char suffix[] = "users.ntd";
            string_util::copy(g_storage_path_fallback + idx,
                              sizeof(g_storage_path_fallback) - idx,
                              suffix);
        }
    }
    g_has_storage_path = (g_storage_path[0] != '\0');
}

bool persist() {
    if (!path_valid()) {
        return false;
    }
    ensure_parent_directories(g_storage_path);
    if (g_storage_path_fallback[0] != '\0') {
        ensure_parent_directories(g_storage_path_fallback);
    }
    Header hdr{};
    hdr.magic = kMagic;
    hdr.version = kVersion;
    hdr.entry_size = static_cast<uint16_t>(sizeof(PackedUser));
    hdr.count = 0;

    PackedUser packed[kMaxUsers];
    for (size_t i = 0; i < kMaxUsers; ++i) {
        const User& u = g_users[i];
        if (!u.active) continue;
        PackedUser& p = packed[hdr.count++];
        memset(&p, 0, sizeof(p));
        string_util::copy(p.name, sizeof(p.name), u.name);
        p.allowed_caps = u.allowed_caps;
        p.generation = u.generation;
        p.active = 1;
    }

    uint8_t buffer[sizeof(Header) + sizeof(PackedUser) * kMaxUsers];
    size_t total = sizeof(Header) + hdr.count * sizeof(PackedUser);
    memcpy(buffer, &hdr, sizeof(Header));
    if (hdr.count > 0) {
        memcpy(buffer + sizeof(Header), packed, hdr.count * sizeof(PackedUser));
    }

    auto write_path = [&](const char* p) -> bool {
        vfs::FileHandle handle{};
        if (!vfs::create_file(p, handle)) {
            if (!vfs::open_file(p, handle)) {
                return false;
            }
        }
        size_t written = 0;
        bool ok = vfs::write_file(handle, 0, buffer, total, written);
        vfs::close_file(handle);
        return ok && written == total;
    };

    if (write_path(g_storage_path)) {
        return true;
    }

    if (g_storage_path_fallback[0] != '\0' &&
        write_path(g_storage_path_fallback)) {
        log_message(LogLevel::Info,
                    "Users: wrote store to fallback path '%s' (primary '%s' missing?)",
                    g_storage_path_fallback, g_storage_path);
        return true;
    }

    log_message(LogLevel::Warn,
                "Users: persist failed for '%s'%s",
                g_storage_path,
                (g_storage_path_fallback[0] != '\0') ? " and fallback" : "");
    return false;
}

bool load_from_disk() {
    if (!path_valid()) {
        return false;
    }
    g_loading_from_disk = true;
    uint8_t buffer[4096];
    size_t read_size = 0;
    const char* loaded_path = nullptr;
    auto try_load = [&](const char* p) -> bool {
        Header hdr{};
        vfs::FileHandle h{};
        size_t local_read_size = 0;
        if (!vfs::open_file(p, h)) {
            return false;
        }
        bool ok = vfs::read_file(h, 0, buffer, sizeof(buffer), local_read_size);
        vfs::close_file(h);
        if (!ok) {
            return false;
        }
        if (local_read_size < sizeof(Header)) {
            return false;
        }
        memcpy(&hdr, buffer, sizeof(Header));
        if (hdr.magic != kMagic || hdr.version != kVersion ||
            hdr.entry_size != sizeof(PackedUser)) {
            return false;
        }
        size_t expected = sizeof(Header) +
                          static_cast<size_t>(hdr.count) * sizeof(PackedUser);
        if (local_read_size < expected) {
            return false;
        }
        read_size = local_read_size;
        loaded_path = p;
        return true;
    };

    bool loaded = try_load(g_storage_path);
    if (!loaded && g_storage_path_fallback[0] != '\0') {
        loaded = try_load(g_storage_path_fallback);
        if (loaded) {
            log_message(LogLevel::Warn,
                        "Users: loaded from fallback path '%s'",
                        g_storage_path_fallback);
        }
    }
    if (!loaded) {
        g_loading_from_disk = false;
        persist();  // create blank store
        return false;
    }
    if (read_size < sizeof(Header)) {
        g_loading_from_disk = false;
        return false;
    }
    Header hdr{};
    memcpy(&hdr, buffer, sizeof(Header));
    init();
    size_t limit = (hdr.count > kMaxUsers) ? kMaxUsers : hdr.count;
    const PackedUser* entries = reinterpret_cast<const PackedUser*>(buffer + sizeof(Header));
    for (size_t i = 0; i < limit; ++i) {
        const PackedUser& p = entries[i];
        User* u = create(p.name, p.allowed_caps);
        if (u != nullptr) {
            u->generation = p.generation;
            u->active = p.active != 0;
            ensure_home_directory(u->name);
        }
    }
    log_message(LogLevel::Info,
                "Users: loaded %u entr%s from '%s'",
                static_cast<unsigned int>(limit),
                (limit == 1) ? "y" : "ies",
                (loaded_path != nullptr) ? loaded_path : "(unknown)");
    g_loading_from_disk = false;
    return true;
}

}  // namespace users
