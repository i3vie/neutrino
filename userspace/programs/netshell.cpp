#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../crt/syscall.hpp"
#include "../auth/password_hash.hpp"
#include "../net/network_protocol.hpp"
#include "../net/tcpd_protocol.hpp"

namespace {

constexpr size_t kMaxConnections = 8;
constexpr uint32_t kStatusPollInterval = 250000;
constexpr uint16_t kDefaultPort = 2222;
constexpr const char* kPrimaryUserStorePath = "/system/users.ntd";
constexpr const char* kFallbackUserStorePath = "/users.ntd";
constexpr size_t kMaxUserNameLength = 32;
constexpr size_t kMaxLoginUsers = 32;
constexpr const char* kRootUserName = "root";
constexpr uint64_t kAllCapabilities = ~0ull;
constexpr uint8_t kTelnetIac = 255;
constexpr uint8_t kTelnetDont = 254;
constexpr uint8_t kTelnetDo = 253;
constexpr uint8_t kTelnetWont = 252;
constexpr uint8_t kTelnetWill = 251;
constexpr uint8_t kTelnetSb = 250;
constexpr uint8_t kTelnetSe = 240;
constexpr uint8_t kTelnetSuppressGoAhead = 3;
constexpr uint8_t kTelnetEcho = 1;

struct Connection {
    bool in_use;
    bool shell_started;
    uint32_t id;
    uint16_t remote_port;
    uint8_t remote_ip[4];
};

struct PrincipalCacheEntry {
    bool in_use;
    char name[kMaxUserNameLength];
    void* principal;
};

PrincipalCacheEntry g_principals[8]{};

struct UserStoreHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t entry_size;
    uint32_t count;
};

struct UserStoreHeaderV3 {
    uint32_t magic;
    uint16_t version;
    uint16_t entry_size;
    uint32_t count;
    uint64_t next_user_id;
    uint64_t machine_id;
};

struct PackedUserV1 {
    char name[kMaxUserNameLength];
    uint64_t allowed_caps;
    uint64_t generation;
    uint8_t active;
    uint8_t reserved[7];
};

struct PackedUser {
    char name[kMaxUserNameLength];
    uint64_t allowed_caps;
    uint64_t generation;
    uint8_t active;
    uint8_t password_set;
    uint16_t password_algorithm;
    uint32_t password_iterations;
    uint8_t password_salt[auth::kPasswordSaltSize];
    uint8_t password_hash[auth::kPasswordHashSize];
    uint8_t reserved[24];
};

struct LoginUsers {
    char names[kMaxLoginUsers][kMaxUserNameLength];
    bool password_set[kMaxLoginUsers];
    uint32_t password_iterations[kMaxLoginUsers];
    uint8_t password_salt[kMaxLoginUsers][auth::kPasswordSaltSize];
    uint8_t password_hash[kMaxLoginUsers][auth::kPasswordHashSize];
    size_t count;
};

constexpr size_t kUserStoreBufferSize =
    sizeof(UserStoreHeaderV3) +
    sizeof(PackedUser) * kMaxLoginUsers + 1;
constexpr uint32_t kMaxPasswordIterations =
    auth::kPasswordIterations * 10u;

struct TelnetSession {
    uint32_t endpoint;
    bool client_echo_suppressed;
    bool in_command;
    bool in_subnegotiation;
    bool subnegotiation_iac;
    uint8_t command;
};

struct NetworkSnapshot {
    uint32_t network_state;
    uint32_t dhcp_state;
    uint32_t net_rx_frames;
    uint32_t net_rx_tcp;
    uint32_t net_rx_delivered;
    uint32_t net_tx_tcp;
    uint32_t tcp_listeners;
    uint32_t tcp_connections;
    uint32_t tcp_rx_segments;
    uint32_t tcp_tx_segments;
    uint32_t tcp_inbound_syns;
    uint32_t tcp_syn_ack_retransmits;
    uint32_t tcp_established;
    uint32_t tcp_wait_ack_mismatch;
    uint32_t tcp_remote_resets;
    uint32_t tcp_remote_fins;
    uint32_t tcp_last_flags;
    uint32_t tcp_last_seq;
    uint32_t tcp_last_ack;
    uint32_t tcp_expected_seq;
    uint32_t tcp_expected_ack;
};

void print(const char* text) {
    static int32_t console = -1;
    if (console < 0) {
        console = static_cast<int32_t>(
            descriptor_open(static_cast<uint32_t>(descriptor_defs::Type::Console), 0));
        if (console < 0) {
            return;
        }
    }
    if (text == nullptr) {
        return;
    }
    size_t len = 0;
    while (text[len] != '\0') {
        ++len;
    }
    if (len != 0) {
        descriptor_write(static_cast<uint32_t>(console), text, len);
    }
}

void print_line(const char* text) {
    print(text);
    print("\n");
}

void zero_memory(void* ptr, size_t size) {
    auto* bytes = static_cast<uint8_t*>(ptr);
    for (size_t i = 0; i < size; ++i) {
        bytes[i] = 0;
    }
}

bool copy_packed_user_name(const char* packed,
                           size_t packed_size,
                           char* out,
                           size_t out_size) {
    if (packed == nullptr || packed_size == 0 || out == nullptr ||
        out_size == 0) {
        return false;
    }
    size_t length = 0;
    while (length < packed_size && packed[length] != '\0') {
        unsigned char ch = static_cast<unsigned char>(packed[length]);
        if (ch < 0x20 || ch == 0x7F || ch == '/' || ch == '\\') {
            return false;
        }
        ++length;
    }
    if (length == 0 || length == packed_size || length >= out_size ||
        (length == 1 && packed[0] == '.') ||
        (length == 2 && packed[0] == '.' && packed[1] == '.')) {
        return false;
    }
    memcpy(out, packed, length);
    out[length] = '\0';
    return true;
}

void endpoint_write(uint32_t endpoint, const char* text) {
    if (text == nullptr) {
        return;
    }
    size_t len = strlen(text);
    size_t written = 0;
    while (written < len) {
        long result = descriptor_write(endpoint, text + written, len - written);
        if (result == kDescriptorWouldBlock) {
            yield();
            continue;
        }
        if (result <= 0) {
            return;
        }
        written += static_cast<size_t>(result);
    }
}

void endpoint_write_bytes(uint32_t endpoint, const uint8_t* data, size_t len) {
    if (data == nullptr) {
        return;
    }
    size_t written = 0;
    while (written < len) {
        long result = descriptor_write(endpoint, data + written, len - written);
        if (result == kDescriptorWouldBlock) {
            yield();
            continue;
        }
        if (result <= 0) {
            return;
        }
        written += static_cast<size_t>(result);
    }
}

void telnet_send(TelnetSession& session, uint8_t command, uint8_t option) {
    uint8_t bytes[3] = {kTelnetIac, command, option};
    endpoint_write_bytes(session.endpoint, bytes, sizeof(bytes));
}

void telnet_start(TelnetSession& session, uint32_t endpoint) {
    session.endpoint = endpoint;
    session.client_echo_suppressed = false;
    session.in_command = false;
    session.in_subnegotiation = false;
    session.subnegotiation_iac = false;
    session.command = 0;
}

void telnet_set_password_mode(TelnetSession& session, bool password_mode) {
    if (password_mode) {
        telnet_send(session, kTelnetWill, kTelnetSuppressGoAhead);
        telnet_send(session, kTelnetWill, kTelnetEcho);
        session.client_echo_suppressed = true;
    } else if (session.client_echo_suppressed) {
        telnet_send(session, kTelnetWont, kTelnetEcho);
        telnet_send(session, kTelnetWont, kTelnetSuppressGoAhead);
        session.client_echo_suppressed = false;
    }
}

void telnet_reply_to_command(TelnetSession& session,
                             uint8_t command,
                             uint8_t option) {
    switch (command) {
        case kTelnetDo:
            telnet_send(session,
                        (option == kTelnetSuppressGoAhead ||
                         (option == kTelnetEcho &&
                          session.client_echo_suppressed)) ? kTelnetWill
                                                            : kTelnetWont,
                        option);
            break;
        case kTelnetDont:
            telnet_send(session, kTelnetWont, option);
            if (option == kTelnetEcho) {
                session.client_echo_suppressed = false;
            }
            break;
        case kTelnetWill:
            telnet_send(session,
                        option == kTelnetSuppressGoAhead ? kTelnetDo
                                                          : kTelnetDont,
                        option);
            break;
        case kTelnetWont:
            telnet_send(session, kTelnetDont, option);
            break;
        default:
            break;
    }
}

bool telnet_filter_byte(TelnetSession& session, uint8_t byte, uint8_t& out) {
    if (session.in_subnegotiation) {
        if (session.subnegotiation_iac) {
            if (byte == kTelnetSe) {
                session.in_subnegotiation = false;
            }
            session.subnegotiation_iac = false;
        } else if (byte == kTelnetIac) {
            session.subnegotiation_iac = true;
        }
        return false;
    }

    if (session.in_command) {
        if (session.command == 0) {
            session.command = byte;
            if (session.command == kTelnetIac) {
                session.in_command = false;
                session.command = 0;
                out = kTelnetIac;
                return true;
            }
            if (session.command == kTelnetSb) {
                session.in_command = false;
                session.command = 0;
                session.in_subnegotiation = true;
            }
            return false;
        }

        telnet_reply_to_command(session, session.command, byte);
        session.in_command = false;
        session.command = 0;
        return false;
    }

    if (byte == kTelnetIac) {
        session.in_command = true;
        session.command = 0;
        return false;
    }

    out = byte;
    return true;
}

void telnet_drain_negotiation(TelnetSession& session) {
    size_t empty_polls = 0;
    for (size_t polls = 0; polls < 128; ++polls) {
        uint8_t buffer[32];
        long result = descriptor_read(session.endpoint, buffer, sizeof(buffer));
        if (result == kDescriptorWouldBlock || result == 0) {
            if (!session.in_command && !session.in_subnegotiation &&
                ++empty_polls >= 8) {
                return;
            }
            yield();
            continue;
        }
        if (result < 0) {
            return;
        }
        empty_polls = 0;
        for (size_t i = 0; i < static_cast<size_t>(result); ++i) {
            uint8_t ignored = 0;
            (void)telnet_filter_byte(session, buffer[i], ignored);
        }
        if (!session.in_command && !session.in_subnegotiation) {
            return;
        }
    }
}

bool endpoint_read_line(TelnetSession& session,
                        char* out,
                        size_t out_size,
                        bool echo) {
    if (out == nullptr || out_size == 0) {
        return false;
    }
    size_t length = 0;
    out[0] = '\0';
    for (;;) {
        uint8_t buffer[32];
        long result = descriptor_read(session.endpoint, buffer, sizeof(buffer));
        if (result == kDescriptorWouldBlock || result == 0) {
            yield();
            continue;
        }
        if (result < 0) {
            return false;
        }
        for (size_t i = 0; i < static_cast<size_t>(result); ++i) {
            uint8_t byte = 0;
            if (!telnet_filter_byte(session, buffer[i], byte)) {
                continue;
            }
            char ch = static_cast<char>(byte);
            if (ch == '\r' || ch == '\n') {
                endpoint_write(session.endpoint, "\r\n");
                out[length] = '\0';
                return true;
            }
            if (ch == '\b' || ch == 0x7F) {
                if (length > 0) {
                    --length;
                    out[length] = '\0';
                    if (echo) {
                        endpoint_write(session.endpoint, "\b \b");
                    }
                }
                continue;
            }
            if (ch < 0x20 || ch > 0x7E) {
                continue;
            }
            if (length + 1 >= out_size) {
                continue;
            }
            out[length++] = ch;
            out[length] = '\0';
            if (echo) {
                char one[2] = {ch, '\0'};
                endpoint_write(session.endpoint, one);
            }
        }
    }
}

bool read_file_into_buffer(const char* path,
                           char* buffer,
                           size_t buffer_size,
                           size_t& out_len) {
    out_len = 0;
    if (path == nullptr || buffer == nullptr || buffer_size == 0) {
        return false;
    }
    long handle = file_open(path);
    if (handle < 0) {
        return false;
    }
    size_t total = 0;
    while (total + 1 < buffer_size) {
        long read = file_read(static_cast<uint32_t>(handle),
                              buffer + total,
                              buffer_size - 1 - total);
        if (read <= 0) {
            break;
        }
        total += static_cast<size_t>(read);
    }
    file_close(static_cast<uint32_t>(handle));
    buffer[total] = '\0';
    out_len = total;
    return total > 0;
}

bool user_exists(const LoginUsers& users, const char* name) {
    if (name == nullptr || name[0] == '\0') {
        return false;
    }
    for (size_t i = 0; i < users.count; ++i) {
        if (strcmp(users.names[i], name) == 0) {
            return true;
        }
    }
    return false;
}

void ensure_login_user(LoginUsers& users, const char* name) {
    if (name == nullptr || name[0] == '\0' || user_exists(users, name)) {
        return;
    }
    if (users.count >= kMaxLoginUsers) {
        return;
    }
    strlcpy(users.names[users.count], name, sizeof(users.names[users.count]));
    users.password_set[users.count] = false;
    users.password_iterations[users.count] = 0;
    zero_memory(users.password_salt[users.count], auth::kPasswordSaltSize);
    zero_memory(users.password_hash[users.count], auth::kPasswordHashSize);
    ++users.count;
}

void set_login_user_password(LoginUsers& users,
                             const char* name,
                             uint32_t iterations,
                             const uint8_t* salt,
                             const uint8_t* hash) {
    if (name == nullptr || salt == nullptr || hash == nullptr || iterations == 0) {
        return;
    }
    for (size_t i = 0; i < users.count; ++i) {
        if (strcmp(users.names[i], name) != 0) {
            continue;
        }
        users.password_set[i] = true;
        users.password_iterations[i] = iterations;
        memcpy(users.password_salt[i], salt, auth::kPasswordSaltSize);
        memcpy(users.password_hash[i], hash, auth::kPasswordHashSize);
        return;
    }
}

bool load_login_users(LoginUsers& users) {
    users.count = 0;
    char buffer[kUserStoreBufferSize];
    size_t len = 0;
    bool loaded = read_file_into_buffer(kPrimaryUserStorePath,
                                        buffer,
                                        sizeof(buffer),
                                        len);
    if (!loaded) {
        loaded = read_file_into_buffer(kFallbackUserStorePath,
                                       buffer,
                                       sizeof(buffer),
                                       len);
    }
    if (!loaded || len < sizeof(UserStoreHeader)) {
        return false;
    }

    UserStoreHeader header{};
    memcpy(&header, buffer, sizeof(header));
    if (header.magic != 0x4E544455u) {
        return false;
    }
    bool current_v2 = header.version == 2 &&
                      header.entry_size == sizeof(PackedUser);
    bool current_v3 = header.version == 3 &&
                      header.entry_size == sizeof(PackedUser);
    bool legacy = header.version == 1 &&
                  header.entry_size == sizeof(PackedUserV1);
    if (!current_v2 && !current_v3 && !legacy) {
        return false;
    }

    size_t available = 0;
    size_t count = header.count;
    if (current_v3) {
        if (len < sizeof(UserStoreHeaderV3)) {
            return false;
        }
        available = (len - sizeof(UserStoreHeaderV3)) / header.entry_size;
    } else {
        available = (len - sizeof(UserStoreHeader)) / header.entry_size;
    }
    if (count > available) {
        return false;
    }
    if (count > kMaxLoginUsers) {
        return false;
    }

    if (legacy) {
        const PackedUserV1* entries =
            reinterpret_cast<const PackedUserV1*>(buffer + sizeof(UserStoreHeader));
        for (size_t i = 0; i < count; ++i) {
            if (entries[i].active == 0) {
                continue;
            }
            char name[kMaxUserNameLength];
            if (!copy_packed_user_name(entries[i].name,
                                       sizeof(entries[i].name),
                                       name,
                                       sizeof(name)) ||
                user_exists(users, name)) {
                return false;
            }
            ensure_login_user(users, name);
        }
    } else {
        const PackedUser* entries = reinterpret_cast<const PackedUser*>(
            buffer + (current_v3 ? sizeof(UserStoreHeaderV3) : sizeof(UserStoreHeader)));
        for (size_t i = 0; i < count; ++i) {
            if (entries[i].active == 0) {
                continue;
            }
            char name[kMaxUserNameLength];
            if (!copy_packed_user_name(entries[i].name,
                                       sizeof(entries[i].name),
                                       name,
                                       sizeof(name)) ||
                user_exists(users, name)) {
                return false;
            }
            ensure_login_user(users, name);
            if (entries[i].password_set != 0) {
                if (entries[i].password_algorithm !=
                        auth::kPasswordAlgorithmPbkdf2Sha256 ||
                    entries[i].password_iterations == 0 ||
                    entries[i].password_iterations > kMaxPasswordIterations) {
                    return false;
                }
                set_login_user_password(users,
                                        name,
                                        entries[i].password_iterations,
                                        entries[i].password_salt,
                                        entries[i].password_hash);
            }
        }
    }

    return users.count != 0;
}

bool find_login_user(const LoginUsers& users,
                     const char* name,
                     size_t& out_index) {
    for (size_t i = 0; i < users.count; ++i) {
        if (strcmp(users.names[i], name) == 0) {
            out_index = i;
            return true;
        }
    }
    return false;
}

bool verify_login_password(TelnetSession& session,
                           const LoginUsers& users,
                           size_t index) {
    if (index >= users.count || !users.password_set[index]) {
        // Network authentication must never accept a passwordless account,
        // including the explicit local bootstrap root account.
        return false;
    }
    telnet_set_password_mode(session, true);
    endpoint_write(session.endpoint, "password: ");
    char password[96];
    if (!endpoint_read_line(session, password, sizeof(password), false)) {
        telnet_set_password_mode(session, false);
        return false;
    }
    telnet_set_password_mode(session, false);
    uint8_t computed[auth::kPasswordHashSize];
    auth::pbkdf2_sha256(password,
                        users.password_salt[index],
                        auth::kPasswordSaltSize,
                        users.password_iterations[index],
                        computed);
    zero_memory(password, sizeof(password));
    bool ok = auth::constant_time_equal(computed,
                                        users.password_hash[index],
                                        auth::kPasswordHashSize);
    zero_memory(computed, sizeof(computed));
    return ok;
}

void* find_cached_principal(const char* user_name) {
    if (user_name == nullptr || user_name[0] == '\0') {
        return nullptr;
    }
    for (size_t i = 0; i < sizeof(g_principals) / sizeof(g_principals[0]); ++i) {
        if (g_principals[i].in_use &&
            strcmp(g_principals[i].name, user_name) == 0) {
            return g_principals[i].principal;
        }
    }
    return nullptr;
}

bool cache_principal(const char* user_name, void* principal) {
    if (user_name == nullptr || user_name[0] == '\0' || principal == nullptr) {
        return false;
    }
    for (size_t i = 0; i < sizeof(g_principals) / sizeof(g_principals[0]); ++i) {
        if (!g_principals[i].in_use) {
            g_principals[i].in_use = true;
            strlcpy(g_principals[i].name,
                    user_name,
                    sizeof(g_principals[i].name));
            g_principals[i].principal = principal;
            return true;
        }
    }
    return false;
}

void* ensure_user_principal(const char* user_name) {
    void* principal = find_cached_principal(user_name);
    if (principal != nullptr) {
        return principal;
    }
    void* user = user_find(user_name);
    if (user == nullptr) {
        return nullptr;
    }
    principal = principal_create(user, kAllCapabilities);
    if (principal == nullptr) {
        return nullptr;
    }
    cache_principal(user_name, principal);
    return principal;
}

void print_u32(uint32_t value) {
    char buf[16];
    size_t pos = 0;
    if (value == 0) {
        buf[pos++] = '0';
    } else {
        char tmp[16];
        size_t count = 0;
        while (value != 0 && count < sizeof(tmp)) {
            tmp[count++] = static_cast<char>('0' + (value % 10));
            value /= 10;
        }
        while (count != 0) {
            buf[pos++] = tmp[--count];
        }
    }
    buf[pos] = '\0';
    print(buf);
}

void print_ip(const uint8_t ip[4]) {
    print_u32(ip[0]);
    print(".");
    print_u32(ip[1]);
    print(".");
    print_u32(ip[2]);
    print(".");
    print_u32(ip[3]);
}

bool parse_u16(const char* text, uint16_t& out) {
    if (text == nullptr || *text == '\0') {
        return false;
    }
    uint32_t value = 0;
    while (*text != '\0') {
        if (*text < '0' || *text > '9') {
            return false;
        }
        value = value * 10u + static_cast<uint32_t>(*text - '0');
        if (value > 65535u) {
            return false;
        }
        ++text;
    }
    out = static_cast<uint16_t>(value);
    return true;
}

bool open_tcpd_registry(uint32_t& handle, tcpd_protocol::Registry*& registry) {
    long shm = shared_memory_open(tcpd_protocol::kRegistryName,
                                  sizeof(tcpd_protocol::Registry));
    if (shm < 0) {
        return false;
    }
    descriptor_defs::SharedMemoryInfo info{};
    if (shared_memory_get_info(static_cast<uint32_t>(shm), &info) != 0 ||
        info.base == 0 ||
        info.length < sizeof(tcpd_protocol::Registry)) {
        descriptor_close(static_cast<uint32_t>(shm));
        return false;
    }
    handle = static_cast<uint32_t>(shm);
    registry = reinterpret_cast<tcpd_protocol::Registry*>(info.base);
    return true;
}

bool open_network_registry(uint32_t& handle,
                           networkd_protocol::Registry*& registry) {
    long shm = shared_memory_open(networkd_protocol::kRegistryName,
                                  sizeof(networkd_protocol::Registry));
    if (shm < 0) {
        return false;
    }
    descriptor_defs::SharedMemoryInfo info{};
    if (shared_memory_get_info(static_cast<uint32_t>(shm), &info) != 0 ||
        info.base == 0 ||
        info.length < sizeof(networkd_protocol::Registry)) {
        descriptor_close(static_cast<uint32_t>(shm));
        return false;
    }
    handle = static_cast<uint32_t>(shm);
    registry = reinterpret_cast<networkd_protocol::Registry*>(info.base);
    return true;
}

bool send_listen_request(uint32_t server_handle,
                         uint32_t reply_pipe_id,
                         uint16_t port) {
    tcpd_protocol::Message message{};
    tcpd_protocol::init_message(message, tcpd_protocol::kListenRequest);
    message.listen_request.reply_pipe_id = reply_pipe_id;
    message.listen_request.port = port;
    return tcpd_protocol::write_message(server_handle, message);
}

bool send_close_request(uint32_t server_handle, uint32_t connection_id) {
    tcpd_protocol::Message message{};
    tcpd_protocol::init_message(message, tcpd_protocol::kCloseRequest);
    message.close_request.connection_id = connection_id;
    return tcpd_protocol::write_message(server_handle, message);
}

Connection* find_connection(Connection* connections, uint32_t id) {
    for (size_t i = 0; i < kMaxConnections; ++i) {
        if (connections[i].in_use && connections[i].id == id) {
            return &connections[i];
        }
    }
    return nullptr;
}

Connection* allocate_connection(Connection* connections) {
    for (size_t i = 0; i < kMaxConnections; ++i) {
        if (!connections[i].in_use) {
            return &connections[i];
        }
    }
    return nullptr;
}

void release_connection(Connection& connection) {
    connection.in_use = false;
    connection.shell_started = false;
    connection.id = 0;
    connection.remote_port = 0;
    connection.remote_ip[0] = 0;
    connection.remote_ip[1] = 0;
    connection.remote_ip[2] = 0;
    connection.remote_ip[3] = 0;
}

void build_shell_args(const char* user_name, char* out, size_t out_size) {
    if (out == nullptr || out_size == 0) {
        return;
    }
    strlcpy(out, "user=", out_size);
    size_t used = strlen(out);
    if (used + 1 >= out_size) {
        return;
    }
    strlcpy(out + used, user_name, out_size - used);
}

bool login_connection(TelnetSession& session,
                      char* user_name,
                      size_t user_name_size) {
    LoginUsers users{};
    if (!load_login_users(users)) {
        endpoint_write(session.endpoint,
                       "Authentication database unavailable\r\n");
        return false;
    }
    endpoint_write(session.endpoint, "\r\nNeutrino telnet\r\nlogin: ");
    char name[kMaxUserNameLength];
    if (!endpoint_read_line(session, name, sizeof(name), false)) {
        return false;
    }
    size_t user_index = 0;
    if (!find_login_user(users, name, user_index)) {
        endpoint_write(session.endpoint, "Unknown user\r\n");
        return false;
    }
    if (!verify_login_password(session, users, user_index)) {
        endpoint_write(session.endpoint, "Login incorrect\r\n");
        return false;
    }
    strlcpy(user_name, name, user_name_size);
    return true;
}

bool spawn_shell_for_connection(uint32_t endpoint_id) {
    if (endpoint_id == 0) {
        return false;
    }
    uint64_t endpoint_flags =
        static_cast<uint64_t>(descriptor_defs::Flag::Readable) |
        static_cast<uint64_t>(descriptor_defs::Flag::Writable) |
        static_cast<uint64_t>(descriptor_defs::Flag::Async);
    long endpoint = net_endpoint_open_existing(endpoint_flags, endpoint_id);
    if (endpoint < 0) {
        return false;
    }

    TelnetSession telnet{};
    telnet_start(telnet, static_cast<uint32_t>(endpoint));
    telnet_drain_negotiation(telnet);
    char user_name[kMaxUserNameLength];
    if (!login_connection(telnet,
                          user_name,
                          sizeof(user_name))) {
        descriptor_close(static_cast<uint32_t>(endpoint));
        return false;
    }
    void* principal = ensure_user_principal(user_name);
    if (principal == nullptr) {
        endpoint_write(static_cast<uint32_t>(endpoint),
                       "Failed to prepare user session\r\n");
        descriptor_close(static_cast<uint32_t>(endpoint));
        return false;
    }

    char shell_args[48];
    build_shell_args(user_name, shell_args, sizeof(shell_args));
    ProcessStdioConfig stdio{};
    stdio.stdin_handle = static_cast<uint32_t>(endpoint);
    stdio.stdout_handle = static_cast<uint32_t>(endpoint);
    stdio.stderr_handle = static_cast<uint32_t>(endpoint);
    stdio.reserved = 0;
    long pid = child_with_stdio_as("/binary/shell.elf",
                                   shell_args,
                                   0,
                                   nullptr,
                                   &stdio,
                                   principal);
    descriptor_close(static_cast<uint32_t>(endpoint));
    return pid >= 0;
}

void close_connection(uint32_t server_pipe, Connection& connection) {
    (void)send_close_request(server_pipe, connection.id);
    release_connection(connection);
}

NetworkSnapshot read_snapshot(const networkd_protocol::Registry* network_registry,
                              const tcpd_protocol::Registry* tcp_registry) {
    NetworkSnapshot snapshot{};
    if (network_registry != nullptr &&
        network_registry->magic == networkd_protocol::kRegistryMagic &&
        network_registry->version == networkd_protocol::kRegistryVersion) {
        snapshot.network_state = network_registry->networkd_state;
        snapshot.dhcp_state = network_registry->dhcp_state;
        snapshot.net_rx_frames = network_registry->net_rx_frames;
        snapshot.net_rx_tcp = network_registry->net_rx_tcp;
        snapshot.net_rx_delivered = network_registry->net_rx_delivered;
        snapshot.net_tx_tcp = network_registry->net_tx_tcp;
    }
    if (tcp_registry != nullptr &&
        tcp_registry->magic == tcpd_protocol::kRegistryMagic &&
        tcp_registry->version == tcpd_protocol::kRegistryVersion) {
        snapshot.tcp_listeners = tcp_registry->listeners;
        snapshot.tcp_connections = tcp_registry->connections;
        snapshot.tcp_rx_segments = tcp_registry->rx_segments;
        snapshot.tcp_tx_segments = tcp_registry->tx_segments;
        snapshot.tcp_inbound_syns = tcp_registry->inbound_syns;
        snapshot.tcp_syn_ack_retransmits = tcp_registry->syn_ack_retransmits;
        snapshot.tcp_established = tcp_registry->established;
        snapshot.tcp_wait_ack_mismatch = tcp_registry->wait_ack_mismatch;
        snapshot.tcp_remote_resets = tcp_registry->remote_resets;
        snapshot.tcp_remote_fins = tcp_registry->remote_fins;
        snapshot.tcp_last_flags = tcp_registry->last_flags;
        snapshot.tcp_last_seq = tcp_registry->last_seq;
        snapshot.tcp_last_ack = tcp_registry->last_ack;
        snapshot.tcp_expected_seq = tcp_registry->expected_seq;
        snapshot.tcp_expected_ack = tcp_registry->expected_ack;
    }
    return snapshot;
}

bool snapshots_equal(const NetworkSnapshot& a, const NetworkSnapshot& b) {
    return a.network_state == b.network_state &&
           a.dhcp_state == b.dhcp_state &&
           a.net_rx_frames == b.net_rx_frames &&
           a.net_rx_tcp == b.net_rx_tcp &&
           a.net_rx_delivered == b.net_rx_delivered &&
           a.net_tx_tcp == b.net_tx_tcp &&
           a.tcp_listeners == b.tcp_listeners &&
           a.tcp_connections == b.tcp_connections &&
           a.tcp_rx_segments == b.tcp_rx_segments &&
           a.tcp_tx_segments == b.tcp_tx_segments &&
           a.tcp_inbound_syns == b.tcp_inbound_syns &&
           a.tcp_syn_ack_retransmits == b.tcp_syn_ack_retransmits &&
           a.tcp_established == b.tcp_established &&
           a.tcp_wait_ack_mismatch == b.tcp_wait_ack_mismatch &&
           a.tcp_remote_resets == b.tcp_remote_resets &&
           a.tcp_remote_fins == b.tcp_remote_fins &&
           a.tcp_last_flags == b.tcp_last_flags &&
           a.tcp_last_seq == b.tcp_last_seq &&
           a.tcp_last_ack == b.tcp_last_ack &&
           a.tcp_expected_seq == b.tcp_expected_seq &&
           a.tcp_expected_ack == b.tcp_expected_ack;
}

void print_snapshot(const NetworkSnapshot& snapshot) {
    print("netshell: net state=");
    print_u32(snapshot.network_state);
    print(" dhcp=");
    print_u32(snapshot.dhcp_state);
    print(" rx_frames=");
    print_u32(snapshot.net_rx_frames);
    print(" rx_tcp=");
    print_u32(snapshot.net_rx_tcp);
    print(" delivered=");
    print_u32(snapshot.net_rx_delivered);
    print(" tx_tcp=");
    print_u32(snapshot.net_tx_tcp);
    print(" tcp listeners=");
    print_u32(snapshot.tcp_listeners);
    print(" conns=");
    print_u32(snapshot.tcp_connections);
    print(" rx_seg=");
    print_u32(snapshot.tcp_rx_segments);
    print(" tx_seg=");
    print_u32(snapshot.tcp_tx_segments);
    print(" syn=");
    print_u32(snapshot.tcp_inbound_syns);
    print(" syn_retx=");
    print_u32(snapshot.tcp_syn_ack_retransmits);
    print(" estab=");
    print_u32(snapshot.tcp_established);
    print(" wait_ack=");
    print_u32(snapshot.tcp_wait_ack_mismatch);
    print(" rst=");
    print_u32(snapshot.tcp_remote_resets);
    print(" fin=");
    print_u32(snapshot.tcp_remote_fins);
    print(" flags=");
    print_u32(snapshot.tcp_last_flags);
    print(" seq=");
    print_u32(snapshot.tcp_last_seq);
    print(" ack=");
    print_u32(snapshot.tcp_last_ack);
    print(" exp_seq=");
    print_u32(snapshot.tcp_expected_seq);
    print(" exp_ack=");
    print_u32(snapshot.tcp_expected_ack);
    print("\n");
}

}  // namespace

extern "C" int main(uint64_t arg_ptr, uint64_t) {
    const char* args = reinterpret_cast<const char*>(arg_ptr);
    print_line("netshell: starting");
    uint16_t port = kDefaultPort;
    if (args != nullptr && args[0] != '\0' && !parse_u16(args, port)) {
        print_line("netshell: invalid port");
        return 1;
    }

    uint32_t registry_handle = 0;
    tcpd_protocol::Registry* registry = nullptr;
    if (!open_tcpd_registry(registry_handle, registry)) {
        print_line("netshell: failed to open tcpd registry");
        return 1;
    }
    while (registry->magic != tcpd_protocol::kRegistryMagic ||
           registry->version != tcpd_protocol::kRegistryVersion ||
           registry->server_pipe_id == 0) {
        yield();
    }

    uint64_t reply_flags = static_cast<uint64_t>(descriptor_defs::Flag::Readable) |
                           static_cast<uint64_t>(descriptor_defs::Flag::Async);
    long reply_pipe = pipe_open_new(reply_flags);
    if (reply_pipe < 0) {
        print_line("netshell: failed to create reply pipe");
        return 1;
    }

    descriptor_defs::PipeInfo info{};
    if (pipe_get_info(static_cast<uint32_t>(reply_pipe), &info) != 0 || info.id == 0) {
        print_line("netshell: failed to query reply pipe");
        return 1;
    }

    uint64_t server_flags = static_cast<uint64_t>(descriptor_defs::Flag::Writable) |
                            static_cast<uint64_t>(descriptor_defs::Flag::Async);
    long server_pipe = pipe_open_existing(server_flags, registry->server_pipe_id);
    if (server_pipe < 0) {
        print_line("netshell: failed to open tcpd pipe");
        return 1;
    }

    if (!send_listen_request(static_cast<uint32_t>(server_pipe), info.id, port)) {
        print_line("netshell: failed to send listen request");
        return 1;
    }

    tcpd_protocol::Message message{};
    while (!tcpd_protocol::read_message(static_cast<uint32_t>(reply_pipe), message)) {
        yield();
    }
    if (message.type != tcpd_protocol::kListenResponse ||
        message.listen_response.status != tcpd_protocol::kStatusOk) {
        print_line("netshell: listen failed");
        return 1;
    }

    Connection connections[kMaxConnections]{};
    print("netshell: listening on ");
    print_u32(port);
    print("\n");

    uint32_t network_registry_handle = 0;
    networkd_protocol::Registry* network_registry = nullptr;
    if (!open_network_registry(network_registry_handle, network_registry)) {
        print_line("netshell: network registry unavailable");
    }
    NetworkSnapshot last_snapshot =
        read_snapshot(network_registry, registry);
    print_snapshot(last_snapshot);
    uint32_t idle_polls = 0;

    for (;;) {
        tcpd_protocol::Message event{};
        if (!tcpd_protocol::read_message(static_cast<uint32_t>(reply_pipe), event)) {
            if (++idle_polls >= kStatusPollInterval) {
                idle_polls = 0;
                NetworkSnapshot snapshot =
                    read_snapshot(network_registry, registry);
                if (!snapshots_equal(snapshot, last_snapshot)) {
                    print_snapshot(snapshot);
                    last_snapshot = snapshot;
                }
            }
            yield();
            continue;
        }
        idle_polls = 0;

        if (event.type == tcpd_protocol::kAcceptEvent) {
            Connection* connection =
                allocate_connection(connections);
            if (connection == nullptr) {
                (void)send_close_request(static_cast<uint32_t>(server_pipe),
                                         event.accept_event.connection_id);
                continue;
            }

            connection->in_use = true;
            connection->shell_started = false;
            connection->id = event.accept_event.connection_id;
            connection->remote_port = event.accept_event.remote_port;
            for (size_t i = 0; i < 4; ++i) {
                connection->remote_ip[i] = event.accept_event.remote_ip[i];
            }

            print("netshell: accept ");
            print_ip(connection->remote_ip);
            print(":");
            print_u32(connection->remote_port);
            print(" id=");
            print_u32(connection->id);
            print("\n");

            if (spawn_shell_for_connection(event.accept_event.endpoint_id)) {
                connection->shell_started = true;
                print_line("netshell: shell started");
            } else {
                print_line("netshell: shell start failed");
                close_connection(static_cast<uint32_t>(server_pipe), *connection);
            }
            continue;
        }

        if (event.type == tcpd_protocol::kDataEvent) {
            continue;
        }

        if (event.type == tcpd_protocol::kClosedEvent) {
            Connection* connection =
                find_connection(connections, event.closed_event.connection_id);
            if (connection != nullptr) {
                print("netshell: closed id=");
                print_u32(connection->id);
                print("\n");
                release_connection(*connection);
            }
        }
    }
}
