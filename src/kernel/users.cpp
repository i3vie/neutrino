#include "users.hpp"

#include <stddef.h>
#include <stdint.h>

#include "capabilities.hpp"
#include "string_util.hpp"
#include "time.hpp"
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
users::UserId g_machine_id = {0, 0};
uint64_t g_next_user_id = 1;

constexpr uint32_t kMagic = 0x4E544455;  // 'NTDU'
constexpr uint16_t kVersion = 3;
constexpr uint16_t kLegacyVersion = 2;
constexpr uint16_t kLegacyVersion1 = 1;
constexpr uint64_t kLegacyAllCapabilities = (1ull << 3) - 1;

struct PackedUserV1 {
    char name[32];
    uint64_t allowed_caps;
    uint64_t generation;
    uint8_t active;
    uint8_t reserved[7];
};

struct PackedUser {
    char name[32];
    uint64_t allowed_caps;
    uint64_t generation;
    uint8_t active;
    uint8_t password_set;
    uint16_t password_algorithm;
    uint32_t password_iterations;
    uint8_t password_salt[16];
    uint8_t password_hash[32];
    uint64_t machine_id;
    uint64_t local_id;
    uint8_t reserved[8];
};

static_assert(sizeof(PackedUserV1) == 56, "PackedUserV1 size changed");
static_assert(sizeof(PackedUser) == 128, "PackedUser size changed");

struct LegacyHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t entry_size;
    uint32_t count;
};

static_assert(sizeof(LegacyHeader) == 12, "LegacyHeader size changed");

struct Header {
    uint32_t magic;
    uint16_t version;
    uint16_t entry_size;
    uint32_t count;
    uint64_t next_user_id;
    uint64_t machine_id;
};

static_assert(sizeof(Header) == 32, "Header size changed");

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

bool machine_id_path(char* out, size_t out_size) {
    return build_root_relative_path("system/machine.id", out, out_size);
}

void ensure_parent_directories(const char* path);

uint64_t random_u64() {
    uint64_t seed = timekeeping::tick_count();
    seed ^= reinterpret_cast<uintptr_t>(&seed);
    if (seed == 0) {
        seed = 0xD0C0FFEE12345678ull;
    }
    uint64_t x = seed;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    return x * 0x2545F4914F6CDD1Dull;
}

bool persist_machine_id() {
    if (!path_valid()) {
        return false;
    }
    char path[128];
    if (!machine_id_path(path, sizeof(path))) {
        return false;
    }
    ensure_parent_directories(path);
    vfs::FileHandle handle{};
    if (!vfs::create_file(path, handle)) {
        if (!vfs::open_file(path, handle)) {
            return false;
        }
    }
    size_t written = 0;
    bool ok = vfs::write_file(handle,
                              0,
                              reinterpret_cast<const uint8_t*>(&g_machine_id.machine),
                              sizeof(g_machine_id.machine),
                              written);
    vfs::close_file(handle);
    return ok && written == sizeof(g_machine_id.machine);
}

bool load_machine_id() {
    if (!path_valid()) {
        return false;
    }
    char path[128];
    if (!machine_id_path(path, sizeof(path))) {
        return false;
    }
    vfs::FileHandle handle{};
    size_t read_size = 0;
    if (vfs::open_file(path, handle)) {
        uint8_t buffer[sizeof(g_machine_id.machine)];
        if (vfs::read_file(handle, 0, buffer, sizeof(buffer), read_size) &&
            read_size == sizeof(buffer)) {
            memcpy(&g_machine_id.machine, buffer, sizeof(g_machine_id.machine));
            vfs::close_file(handle);
            if (g_machine_id.machine != 0) {
                return true;
            }
        }
        vfs::close_file(handle);
    }
    g_machine_id.machine = random_u64();
    if (g_machine_id.machine == 0) {
        g_machine_id.machine = 1;
    }
    return persist_machine_id();
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

uint64_t upgrade_legacy_caps(uint64_t stored_caps, bool& upgraded) {
    if (stored_caps == capabilities::kFullPermissions) {
        return stored_caps;
    }
    if (stored_caps == kLegacyAllCapabilities) {
        upgraded = true;
        return capabilities::kFullPermissions;
    }
    uint64_t normalized = capabilities::normalize_mask(stored_caps);
    if (normalized != stored_caps) {
        upgraded = true;
    }
    return normalized;
}

}  // namespace

namespace users {

void init() {
    for (size_t i = 0; i < kMaxUsers; ++i) {
        g_users[i].active = false;
        g_users[i].allowed_caps = 0;
        g_users[i].generation = 0;
        g_users[i].password_iterations = 0;
        g_users[i].password_set = false;
        memset(g_users[i].password_salt, 0, sizeof(g_users[i].password_salt));
        memset(g_users[i].password_hash, 0, sizeof(g_users[i].password_hash));
        g_users[i].name[0] = '\0';
        g_users[i].id.machine = 0;
        g_users[i].id.local = 0;
    }
    if (g_machine_id.machine == 0 && path_valid()) {
        load_machine_id();
    }
    if (g_next_user_id == 0) {
        g_next_user_id = 1;
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
    allowed_caps = capabilities::normalize_mask(allowed_caps);
    if (find(name) != nullptr) {
        return nullptr;  // already exists
    }
    if (g_machine_id.machine == 0 && path_valid()) {
        load_machine_id();
    }
    for (size_t i = 0; i < kMaxUsers; ++i) {
        User& u = g_users[i];
        if (u.active) {
            continue;
        }
        string_util::copy(u.name, sizeof(u.name), name);
        u.allowed_caps = allowed_caps;
        u.generation = 1;
        u.password_iterations = 0;
        u.password_set = false;
        memset(u.password_salt, 0, sizeof(u.password_salt));
        memset(u.password_hash, 0, sizeof(u.password_hash));
        u.id.machine = g_machine_id.machine;
        u.id.local = g_next_user_id++;
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

User* find(const UserId& id) {
    if (id.local == 0) {
        return nullptr;
    }
    for (size_t i = 0; i < kMaxUsers; ++i) {
        User& u = g_users[i];
        if (!u.active) {
            continue;
        }
        if (u.id.machine == id.machine && u.id.local == id.local) {
            return &u;
        }
    }
    return nullptr;
}

uint64_t handle_for(const User* user) {
    if (user == nullptr || !user->active || user->generation == 0) {
        return 0;
    }
    ptrdiff_t index = user - &g_users[0];
    if (index < 0 || static_cast<size_t>(index) >= kMaxUsers) {
        return 0;
    }
    return (user->generation << 32) |
           (static_cast<uint64_t>(index) + 1u);
}

User* from_handle(uint64_t handle) {
    uint32_t encoded_index = static_cast<uint32_t>(handle);
    uint64_t generation = handle >> 32;
    if (encoded_index == 0 || generation == 0) {
        return nullptr;
    }
    size_t index = static_cast<size_t>(encoded_index - 1u);
    if (index >= kMaxUsers) {
        return nullptr;
    }
    User& user = g_users[index];
    if (!user.active || user.generation != generation) {
        return nullptr;
    }
    return &user;
}

const UserId& machine_id() {
    return g_machine_id;
}

void bump_generation(User& user) {
    if (!user.active) {
        return;
    }
    ++user.generation;
    if (user.generation == 0) {
        user.generation = 1;
    }
    // No persistence required; tokens are ephemeral across restarts.
}

bool allows(const User& user, uint64_t cap_bitmask) {
    if (!user.active) {
        return false;
    }
    return capabilities::mask_allows(user.allowed_caps, cap_bitmask);
}

bool set_password(User& user,
                  const uint8_t* salt,
                  const uint8_t* hash,
                  uint32_t iterations) {
    if (!user.active || salt == nullptr || hash == nullptr || iterations == 0) {
        return false;
    }
    memcpy(user.password_salt, salt, sizeof(user.password_salt));
    memcpy(user.password_hash, hash, sizeof(user.password_hash));
    user.password_iterations = iterations;
    user.password_set = true;
    return persist();
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
    if (g_machine_id.machine == 0) {
        load_machine_id();
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
    hdr.next_user_id = g_next_user_id;
    hdr.machine_id = g_machine_id.machine;

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
        p.password_set = u.password_set ? 1 : 0;
        p.password_algorithm = u.password_set ? 1 : 0;
        p.password_iterations = u.password_iterations;
        memcpy(p.password_salt, u.password_salt, sizeof(p.password_salt));
        memcpy(p.password_hash, u.password_hash, sizeof(p.password_hash));
        p.machine_id = u.id.machine;
        p.local_id = u.id.local;
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
    load_machine_id();

    uint8_t buffer[4096];
    size_t read_size = 0;
    const char* loaded_path = nullptr;
    auto try_load = [&](const char* p) -> bool {
        LegacyHeader raw_hdr{};
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
        if (local_read_size < sizeof(LegacyHeader)) {
            return false;
        }
        memcpy(&raw_hdr, buffer, sizeof(LegacyHeader));
        if (raw_hdr.magic != kMagic) {
            return false;
        }
        bool current = raw_hdr.version == kVersion &&
                       raw_hdr.entry_size == sizeof(PackedUser);
        bool legacy = raw_hdr.version == kLegacyVersion &&
                      raw_hdr.entry_size == sizeof(PackedUser);
        bool legacy_v1 = raw_hdr.version == kLegacyVersion1 &&
                         raw_hdr.entry_size == sizeof(PackedUserV1);
        if (!current && !legacy && !legacy_v1) {
            return false;
        }
        size_t expected = sizeof(LegacyHeader) +
                          static_cast<size_t>(raw_hdr.count) * raw_hdr.entry_size;
        if (current) {
            expected = sizeof(Header) +
                       static_cast<size_t>(raw_hdr.count) * raw_hdr.entry_size;
        }
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
        // Missing and malformed credential stores must remain distinguishable
        // from an explicitly provisioned empty store.  In particular, do not
        // overwrite corruption with a passwordless bootstrap database.
        return false;
    }
    if (read_size < sizeof(LegacyHeader)) {
        g_loading_from_disk = false;
        return false;
    }

    LegacyHeader raw_hdr{};
    memcpy(&raw_hdr, buffer, sizeof(LegacyHeader));
    Header hdr{};
    if (raw_hdr.version == kVersion) {
        if (read_size < sizeof(Header)) {
            g_loading_from_disk = false;
            return false;
        }
        memcpy(&hdr, buffer, sizeof(Header));
        if (hdr.machine_id == 0) {
            hdr.machine_id = g_machine_id.machine;
        }
    } else {
        hdr.magic = raw_hdr.magic;
        hdr.version = raw_hdr.version;
        hdr.entry_size = raw_hdr.entry_size;
        hdr.count = raw_hdr.count;
        hdr.next_user_id = 1;
        hdr.machine_id = g_machine_id.machine;
    }

    init();
    bool upgraded_legacy_entries = false;
    if (hdr.version == kLegacyVersion1) {
        const PackedUserV1* entries =
            reinterpret_cast<const PackedUserV1*>(buffer + sizeof(LegacyHeader));
        for (size_t i = 0; i < hdr.count && i < kMaxUsers; ++i) {
            const PackedUserV1& p = entries[i];
            uint64_t allowed_caps = upgrade_legacy_caps(p.allowed_caps,
                                                        upgraded_legacy_entries);
            User* u = create(p.name, allowed_caps);
            if (u != nullptr) {
                u->generation = p.generation;
                u->active = p.active != 0;
                if (u->id.local == 0) {
                    u->id.local = g_next_user_id++;
                }
                if (u->id.machine == 0) {
                    u->id.machine = hdr.machine_id;
                }
                ensure_home_directory(u->name);
            }
        }
    } else {
        const PackedUser* entries =
            reinterpret_cast<const PackedUser*>(buffer +
                                               ((hdr.version == kVersion)
                                                    ? sizeof(Header)
                                                    : sizeof(LegacyHeader)));
        for (size_t i = 0; i < hdr.count && i < kMaxUsers; ++i) {
            const PackedUser& p = entries[i];
            uint64_t allowed_caps = upgrade_legacy_caps(p.allowed_caps,
                                                        upgraded_legacy_entries);
            User* u = create(p.name, allowed_caps);
            if (u != nullptr) {
                u->generation = p.generation;
                u->active = p.active != 0;
                if (p.password_set != 0 && p.password_algorithm == 1 &&
                    p.password_iterations != 0) {
                    u->password_set = true;
                    u->password_iterations = p.password_iterations;
                    memcpy(u->password_salt,
                           p.password_salt,
                           sizeof(u->password_salt));
                    memcpy(u->password_hash,
                           p.password_hash,
                           sizeof(u->password_hash));
                }
                u->id.machine = (p.machine_id != 0) ? p.machine_id : hdr.machine_id;
                u->id.local = p.local_id;
                if (u->id.local == 0) {
                    u->id.local = g_next_user_id++;
                }
                if (u->id.machine == 0) {
                    u->id.machine = hdr.machine_id;
                }
                if (u->id.local >= g_next_user_id) {
                    g_next_user_id = u->id.local + 1;
                }
                ensure_home_directory(u->name);
            }
        }
    }

    log_message(LogLevel::Info,
                "Users: loaded %u entr%s from '%s'",
                static_cast<unsigned int>(hdr.count),
                (hdr.count == 1) ? "y" : "ies",
                (loaded_path != nullptr) ? loaded_path : "(unknown)");
    if (upgraded_legacy_entries || hdr.version != kVersion) {
        log_message(LogLevel::Info,
                    "Users: upgraded legacy user store to current format");
    }
    g_loading_from_disk = false;
    if (upgraded_legacy_entries || hdr.version != kVersion) {
        persist();
    }
    return true;
}

}  // namespace users
