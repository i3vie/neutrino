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

    // Ensure parent directory exists if path contains subdirectories.
    auto ensure_parent_dir = [](const char* path) {
        char dir[path_util::kMaxPathLength];
        string_util::copy(dir, sizeof(dir), path);
        // strip filename
        for (int i = static_cast<int>(string_util::length(dir)) - 1; i >= 0; --i) {
            if (dir[i] == '/') {
                dir[i] = '\0';
                break;
            }
        }
        if (dir[0] == '\0') {
            return;
        }
        // Try to open the directory to see if it exists
        vfs::DirectoryHandle dh{};
        if (vfs::open_directory(dir, dh)) {
            vfs::close_directory(dh);
            return;
        }
        // No mkdir capability yet; if missing, persist will still fail and log.
    };

    ensure_parent_dir(g_storage_path);
    if (g_storage_path_fallback[0] != '\0') {
        ensure_parent_dir(g_storage_path_fallback);
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
    size_t read = 0;
    vfs::FileHandle handle{};
    auto try_load = [&](const char* p) -> bool {
        Header hdr{};
        vfs::FileHandle h{};
        size_t read_size = 0;
        if (!vfs::open_file(p, h)) {
            return false;
        }
        bool ok = vfs::read_file(h, 0, buffer, sizeof(buffer), read_size);
        vfs::close_file(h);
        if (!ok) {
            return false;
        }
        if (read_size < sizeof(Header)) {
            return false;
        }
        memcpy(&hdr, buffer, sizeof(Header));
        if (hdr.magic != kMagic || hdr.version != kVersion ||
            hdr.entry_size != sizeof(PackedUser)) {
            return false;
        }
        size_t expected = sizeof(Header) +
                          static_cast<size_t>(hdr.count) * sizeof(PackedUser);
        if (read_size < expected) {
            return false;
        }
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
    if (!vfs::read_file(handle, 0, buffer, sizeof(buffer), read)) {
        vfs::close_file(handle);
        g_loading_from_disk = false;
        return false;
    }
    vfs::close_file(handle);
    if (read < sizeof(Header)) {
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
        }
    }
    g_loading_from_disk = false;
    return true;
}

}  // namespace users
