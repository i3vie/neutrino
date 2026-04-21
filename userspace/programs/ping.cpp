#include <stddef.h>
#include <stdint.h>

#include "../crt/syscall.hpp"
#include "../net/network_protocol.hpp"

namespace {

constexpr size_t kPayloadSize = 32;
constexpr size_t kAttemptsDefault = 4;
constexpr size_t kReplyPollSpins = 20000;

int g_registry_fail_reason = 0;

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

void print_ipv4(const uint8_t ip[4]) {
    print_u32(ip[0]); print(".");
    print_u32(ip[1]); print(".");
    print_u32(ip[2]); print(".");
    print_u32(ip[3]);
}

bool parse_u32(const char* text, uint32_t& out) {
    if (text == nullptr || *text == '\0') {
        return false;
    }
    uint32_t value = 0;
    while (*text != '\0') {
        if (*text < '0' || *text > '9') {
            return false;
        }
        value = value * 10u + static_cast<uint32_t>(*text - '0');
        ++text;
    }
    out = value;
    return true;
}

bool parse_ipv4(const char* text, uint8_t out[4]) {
    if (text == nullptr || out == nullptr) {
        return false;
    }
    uint32_t parts[4] = {};
    size_t part = 0;
    uint32_t current = 0;
    bool have_digit = false;
    while (*text != '\0') {
        char ch = *text++;
        if (ch >= '0' && ch <= '9') {
            current = current * 10u + static_cast<uint32_t>(ch - '0');
            if (current > 255u) {
                return false;
            }
            have_digit = true;
            continue;
        }
        if (ch != '.' || !have_digit || part >= 3) {
            return false;
        }
        parts[part++] = current;
        current = 0;
        have_digit = false;
    }
    if (!have_digit || part != 3) {
        return false;
    }
    parts[3] = current;
    for (size_t i = 0; i < 4; ++i) {
        out[i] = static_cast<uint8_t>(parts[i]);
    }
    return true;
}

struct Tokens {
    const char* start[4];
    size_t length[4];
    size_t count;
};

Tokens tokenize(const char* args) {
    Tokens out{};
    const char* cur = args;
    while (cur != nullptr && *cur != '\0' && out.count < 4) {
        while (*cur == ' ') {
            ++cur;
        }
        if (*cur == '\0') {
            break;
        }
        out.start[out.count] = cur;
        while (*cur != '\0' && *cur != ' ') {
            ++out.length[out.count];
            ++cur;
        }
        ++out.count;
    }
    return out;
}

void copy_token(const Tokens& tokens, size_t index, char* out, size_t out_size) {
    if (out == nullptr || out_size == 0) {
        return;
    }
    if (index >= tokens.count) {
        out[0] = '\0';
        return;
    }
    size_t n = (tokens.length[index] < out_size - 1) ? tokens.length[index] : out_size - 1;
    for (size_t i = 0; i < n; ++i) {
        out[i] = tokens.start[index][i];
    }
    out[n] = '\0';
}

descriptor_defs::SharedMemoryInfo* allocate_shm_info_buffer() {
    auto* info = static_cast<descriptor_defs::SharedMemoryInfo*>(
        map_anonymous(sizeof(descriptor_defs::SharedMemoryInfo), MAP_WRITE));
    if (info != nullptr) {
        info->base = 0;
        info->length = 0;
    }
    return info;
}

descriptor_defs::PipeInfo* allocate_pipe_info_buffer() {
    auto* info = static_cast<descriptor_defs::PipeInfo*>(
        map_anonymous(sizeof(descriptor_defs::PipeInfo), MAP_WRITE));
    if (info != nullptr) {
        info->id = 0;
        info->flags = 0;
    }
    return info;
}

bool open_registry(uint32_t& handle, networkd_protocol::Registry*& registry) {
    g_registry_fail_reason = 0;
    long shm = shared_memory_open(networkd_protocol::kRegistryName,
                                  sizeof(networkd_protocol::Registry));
    if (shm < 0) {
        g_registry_fail_reason = 1;
        return false;
    }
    auto* info = allocate_shm_info_buffer();
    if (info == nullptr) {
        g_registry_fail_reason = 2;
        descriptor_close(static_cast<uint32_t>(shm));
        return false;
    }
    long info_result = shared_memory_get_info(static_cast<uint32_t>(shm), info);
    uint64_t base = info->base;
    uint64_t length = info->length;
    unmap(info, sizeof(*info));
    if (info_result != 0 || base == 0 ||
        length < sizeof(networkd_protocol::Registry)) {
        g_registry_fail_reason = 3;
        descriptor_close(static_cast<uint32_t>(shm));
        return false;
    }
    handle = static_cast<uint32_t>(shm);
    registry = reinterpret_cast<networkd_protocol::Registry*>(base);
    return true;
}

bool send_ping_request(uint32_t server_handle,
                       uint32_t reply_pipe_id,
                       const uint8_t destination_ip[4],
                       uint16_t identifier,
                       uint16_t sequence,
                       const uint8_t* payload,
                       size_t payload_length) {
    networkd_protocol::Message message{};
    networkd_protocol::init_message(message, networkd_protocol::kSendIcmpEchoRequest);
    message.icmp_request.reply_pipe_id = reply_pipe_id;
    message.icmp_request.identifier = identifier;
    message.icmp_request.sequence = sequence;
    message.icmp_request.payload_length = static_cast<uint16_t>(payload_length);
    for (size_t i = 0; i < 4; ++i) {
        message.icmp_request.destination_ip[i] = destination_ip[i];
    }
    for (size_t i = 0; i < payload_length; ++i) {
        message.icmp_request.payload[i] = payload[i];
    }
    return networkd_protocol::write_message(server_handle, message);
}

bool wait_for_reply(uint32_t reply_handle,
                    uint16_t identifier,
                    uint16_t sequence,
                    networkd_protocol::IcmpEchoReplyEvent& out) {
    for (size_t spins = 0; spins < kReplyPollSpins; ++spins) {
        networkd_protocol::Message message{};
        if (!networkd_protocol::read_message(reply_handle, message)) {
            yield();
            continue;
        }
        if (message.type != networkd_protocol::kIcmpEchoReplyEvent) {
            continue;
        }
        if (message.icmp_event.identifier == identifier &&
            message.icmp_event.sequence == sequence) {
            out = message.icmp_event;
            return true;
        }
    }
    return false;
}

void print_usage() {
    print_line("usage: ping <ipv4> [count]");
}

}  // namespace

extern "C" int main(uint64_t arg_ptr, uint64_t) {
    const char* args = reinterpret_cast<const char*>(arg_ptr);
    Tokens tokens = tokenize(args);
    if (tokens.count == 0) {
        print_usage();
        return 1;
    }

    char ip_text[32];
    copy_token(tokens, 0, ip_text, sizeof(ip_text));
    uint8_t destination_ip[4];
    if (!parse_ipv4(ip_text, destination_ip)) {
        print_line("ping: invalid IPv4 address");
        return 1;
    }

    size_t attempt_count = kAttemptsDefault;
    if (tokens.count >= 2) {
        char count_text[16];
        copy_token(tokens, 1, count_text, sizeof(count_text));
        uint32_t parsed = 0;
        if (!parse_u32(count_text, parsed) || parsed == 0) {
            print_line("ping: invalid count");
            return 1;
        }
        attempt_count = parsed;
    }

    uint32_t registry_handle = 0;
    networkd_protocol::Registry* registry = nullptr;
    if (!open_registry(registry_handle, registry)) {
        print("ping: failed to open network registry reason=");
        print_u32(static_cast<uint32_t>(g_registry_fail_reason));
        print("\n");
        return 1;
    }
    while (registry->magic != networkd_protocol::kRegistryMagic ||
           registry->version != networkd_protocol::kRegistryVersion ||
           registry->server_pipe_id == 0) {
        yield();
    }

    uint64_t reply_flags = static_cast<uint64_t>(descriptor_defs::Flag::Readable) |
                           static_cast<uint64_t>(descriptor_defs::Flag::Async);
    long reply_pipe = pipe_open_new(reply_flags);
    if (reply_pipe < 0) {
        print_line("ping: failed to create reply pipe");
        return 1;
    }
    auto* reply_info = allocate_pipe_info_buffer();
    if (reply_info == nullptr) {
        print_line("ping: failed to allocate reply pipe info buffer");
        return 1;
    }
    if (pipe_get_info(static_cast<uint32_t>(reply_pipe), reply_info) != 0 ||
        reply_info->id == 0) {
        print_line("ping: failed to query reply pipe");
        unmap(reply_info, sizeof(*reply_info));
        return 1;
    }
    uint32_t reply_pipe_id = reply_info->id;
    unmap(reply_info, sizeof(*reply_info));

    uint64_t server_flags = static_cast<uint64_t>(descriptor_defs::Flag::Writable) |
                            static_cast<uint64_t>(descriptor_defs::Flag::Async);
    long server_pipe = pipe_open_existing(server_flags, registry->server_pipe_id);
    if (server_pipe < 0) {
        print_line("ping: failed to open networkd pipe");
        return 1;
    }

    uint8_t payload[kPayloadSize];
    for (size_t i = 0; i < sizeof(payload); ++i) {
        payload[i] = static_cast<uint8_t>('a' + (i % 26));
    }
    uint16_t identifier = 0x4E50;
    size_t received = 0;

    for (size_t i = 0; i < attempt_count; ++i) {
        uint16_t sequence = static_cast<uint16_t>(i);
        if (!send_ping_request(static_cast<uint32_t>(server_pipe),
                               reply_pipe_id,
                               destination_ip,
                               identifier,
                               sequence,
                               payload,
                               sizeof(payload))) {
            print_line("ping: send failed");
            return 1;
        }

        networkd_protocol::IcmpEchoReplyEvent reply{};
        if (wait_for_reply(static_cast<uint32_t>(reply_pipe),
                           identifier,
                           sequence,
                           reply)) {
            print_u32(reply.payload_length);
            print(" bytes from ");
            print_ipv4(reply.source_ip);
            print(" icmp_seq=");
            print_u32(reply.sequence);
            print(" ttl=");
            print_u32(reply.ttl);
            print("\n");
            ++received;
        } else {
            print("timeout from ");
            print_ipv4(destination_ip);
            print(" icmp_seq=");
            print_u32(sequence);
            print("\n");
        }

        for (size_t delay = 0; delay < 30000; ++delay) {
            yield();
        }
    }

    print("--- ping statistics ---\n");
    print_u32(static_cast<uint32_t>(attempt_count));
    print(" transmitted, ");
    print_u32(static_cast<uint32_t>(received));
    print(" received\n");
    return (received == 0) ? 1 : 0;
}
