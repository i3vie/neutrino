#pragma once

#include <stddef.h>
#include <stdint.h>

#include "../crt/syscall.hpp"
#include "network_protocol.hpp"
#include "udp.hpp"

namespace usernet::dns {

constexpr uint16_t kDnsPort = 53;
constexpr uint16_t kFirstEphemeralPort = 53000;
constexpr uint16_t kEphemeralPortCount = 32;
constexpr size_t kRegistryPollSpins = 120000;
constexpr size_t kReplyPollSpins = 60000;
constexpr size_t kMaxDnsMessageSize = 512;
constexpr size_t kResolveAttempts = 3;

enum class ResolveStatus : uint8_t {
    Ok,
    NetworkRegistryUnavailable,
    NetworkRegistryTimeout,
    NoIpv4Config,
    NoDnsServer,
    ServerPipeUnavailable,
    ReplyPipeUnavailable,
    BindFailed,
    QuerySendFailed,
    NoResponse,
};

struct ResolverContext {
    uint32_t network_registry_handle = kInvalidDescriptor;
    uint32_t server_pipe = kInvalidDescriptor;
    uint32_t reply_pipe = kInvalidDescriptor;
    uint16_t source_port = 0;
    uint8_t source_ip[4]{};
    uint8_t dns_server[4]{};
};

static ResolveStatus g_last_status = ResolveStatus::Ok;

inline ResolveStatus last_status() {
    return g_last_status;
}

inline const char* status_text(ResolveStatus status) {
    switch (status) {
        case ResolveStatus::Ok:
            return "ok";
        case ResolveStatus::NetworkRegistryUnavailable:
            return "network registry unavailable";
        case ResolveStatus::NetworkRegistryTimeout:
            return "network registry timeout";
        case ResolveStatus::NoIpv4Config:
            return "no IPv4 config";
        case ResolveStatus::NoDnsServer:
            return "no DNS server configured";
        case ResolveStatus::ServerPipeUnavailable:
            return "networkd pipe unavailable";
        case ResolveStatus::ReplyPipeUnavailable:
            return "DNS reply pipe unavailable";
        case ResolveStatus::BindFailed:
            return "UDP bind failed";
        case ResolveStatus::QuerySendFailed:
            return "DNS query send failed";
        case ResolveStatus::NoResponse:
            return "no DNS response";
    }
    return "unknown DNS failure";
}

inline descriptor_defs::SharedMemoryInfo* allocate_shm_info_buffer() {
    auto* info = static_cast<descriptor_defs::SharedMemoryInfo*>(
        map_anonymous(sizeof(descriptor_defs::SharedMemoryInfo), MAP_WRITE));
    if (info != nullptr) {
        info->base = 0;
        info->length = 0;
    }
    return info;
}

inline descriptor_defs::PipeInfo* allocate_pipe_info_buffer() {
    auto* info = static_cast<descriptor_defs::PipeInfo*>(
        map_anonymous(sizeof(descriptor_defs::PipeInfo), MAP_WRITE));
    if (info != nullptr) {
        info->id = 0;
        info->flags = 0;
    }
    return info;
}

inline networkd_protocol::Message* allocate_message_buffer() {
    auto* buffer = static_cast<networkd_protocol::Message*>(
        map_anonymous(sizeof(networkd_protocol::Message), MAP_WRITE));
    if (buffer != nullptr) {
        for (size_t i = 0; i < sizeof(*buffer); ++i) {
            reinterpret_cast<uint8_t*>(buffer)[i] = 0;
        }
    }
    return buffer;
}

inline uint8_t* allocate_query_buffer() {
    auto* buffer = static_cast<uint8_t*>(
        map_anonymous(kMaxDnsMessageSize, MAP_WRITE));
    if (buffer != nullptr) {
        for (size_t i = 0; i < kMaxDnsMessageSize; ++i) {
            buffer[i] = 0;
        }
    }
    return buffer;
}

inline bool ipv4_is_zero(const uint8_t ip[4]) {
    return ip[0] == 0 && ip[1] == 0 && ip[2] == 0 && ip[3] == 0;
}

inline bool unbind_udp(uint32_t server_handle, uint16_t port);

inline void close_context(ResolverContext& ctx) {
    if (ctx.server_pipe != kInvalidDescriptor && ctx.source_port != 0) {
        (void)unbind_udp(ctx.server_pipe, ctx.source_port);
        ctx.source_port = 0;
    }
    if (ctx.server_pipe != kInvalidDescriptor) {
        descriptor_close(ctx.server_pipe);
        ctx.server_pipe = kInvalidDescriptor;
    }
    if (ctx.reply_pipe != kInvalidDescriptor) {
        descriptor_close(ctx.reply_pipe);
        ctx.reply_pipe = kInvalidDescriptor;
    }
    if (ctx.network_registry_handle != kInvalidDescriptor) {
        descriptor_close(ctx.network_registry_handle);
        ctx.network_registry_handle = kInvalidDescriptor;
    }
}

inline bool open_registry(uint32_t& handle, networkd_protocol::Registry*& registry) {
    long shm = shared_memory_open(networkd_protocol::kRegistryName,
                                  sizeof(networkd_protocol::Registry));
    if (shm < 0) {
        return false;
    }
    auto* info = allocate_shm_info_buffer();
    if (info == nullptr) {
        descriptor_close(static_cast<uint32_t>(shm));
        return false;
    }
    long info_result =
        shared_memory_get_info(static_cast<uint32_t>(shm), info);
    uint64_t info_base = info->base;
    uint64_t info_length = info->length;
    unmap(info, sizeof(*info));
    if (info_result != 0 ||
        info_base == 0 ||
        info_length < sizeof(networkd_protocol::Registry)) {
        descriptor_close(static_cast<uint32_t>(shm));
        return false;
    }
    handle = static_cast<uint32_t>(shm);
    registry = reinterpret_cast<networkd_protocol::Registry*>(info_base);
    return true;
}

inline bool bind_udp(uint32_t server_handle, uint32_t reply_pipe_id, uint16_t port) {
    auto* request = allocate_message_buffer();
    if (request == nullptr) {
        return false;
    }
    networkd_protocol::init_message(*request, networkd_protocol::kBindUdpRequest);
    request->bind_request.reply_pipe_id = reply_pipe_id;
    request->bind_request.port = port;
    bool ok = networkd_protocol::write_message(server_handle, *request);
    unmap(request, sizeof(*request));
    return ok;
}

inline bool unbind_udp(uint32_t server_handle, uint16_t port) {
    auto* request = allocate_message_buffer();
    if (request == nullptr) {
        return false;
    }
    networkd_protocol::init_message(*request, networkd_protocol::kUnbindUdpRequest);
    request->unbind_request.port = port;
    bool ok = networkd_protocol::write_message(server_handle, *request);
    unmap(request, sizeof(*request));
    return ok;
}

inline bool wait_for_bind_response(uint32_t reply_handle,
                                   uint16_t port,
                                   int32_t& status_out) {
    for (size_t spins = 0; spins < kReplyPollSpins; ++spins) {
        auto* message = allocate_message_buffer();
        if (message == nullptr) {
            return false;
        }
        if (!networkd_protocol::read_message(reply_handle, *message)) {
            unmap(message, sizeof(*message));
            yield();
            continue;
        }
        if (message->type != networkd_protocol::kBindUdpResponse ||
            message->bind_response.port != port) {
            unmap(message, sizeof(*message));
            continue;
        }
        status_out = message->bind_response.status;
        unmap(message, sizeof(*message));
        return true;
    }
    return false;
}

inline bool send_udp(uint32_t server_handle,
                     uint16_t source_port,
                     const uint8_t source_ip[4],
                     uint16_t destination_port,
                     const uint8_t destination_ip[4],
                     const void* payload,
                     size_t payload_length) {
    if (payload == nullptr || payload_length > networkd_protocol::kMaxUdpPayload) {
        return false;
    }

    auto* request = allocate_message_buffer();
    if (request == nullptr) {
        return false;
    }
    networkd_protocol::init_message(*request, networkd_protocol::kSendUdpRequest);
    request->send_request.source_port = source_port;
    request->send_request.destination_port = destination_port;
    request->send_request.payload_length = static_cast<uint16_t>(payload_length);
    for (size_t i = 0; i < 4; ++i) {
        request->send_request.source_ip[i] = source_ip[i];
        request->send_request.destination_ip[i] = destination_ip[i];
    }
    const uint8_t* bytes = static_cast<const uint8_t*>(payload);
    for (size_t i = 0; i < payload_length; ++i) {
        request->send_request.payload[i] = bytes[i];
    }
    bool ok = networkd_protocol::write_message(server_handle, *request);
    unmap(request, sizeof(*request));
    return ok;
}

inline uint16_t hash_host(const char* host) {
    uint16_t hash = 0x4E54u;
    for (const char* p = host; p != nullptr && *p != '\0'; ++p) {
        hash = static_cast<uint16_t>((hash << 5) ^ (hash >> 11) ^
                                     static_cast<uint8_t>(*p));
    }
    return hash;
}

inline uint16_t make_query_id(const char* host) {
    static uint16_t sequence = 0;
    sequence = static_cast<uint16_t>(sequence + 1);
    uint16_t hash = hash_host(host);
    uint16_t query_id = static_cast<uint16_t>(hash ^ sequence);
    return (query_id != 0) ? query_id : 1;
}

inline bool append_dns_name(uint8_t* out,
                            size_t capacity,
                            size_t& offset,
                            const char* host) {
    if (out == nullptr || host == nullptr) {
        return false;
    }
    const char* label = host;
    size_t label_length = 0;
    for (const char* p = host;; ++p) {
        char ch = *p;
        if (ch == '.' || ch == '\0') {
            if (label_length == 0 || label_length > 63 || offset + 1 + label_length > capacity) {
                return false;
            }
            out[offset++] = static_cast<uint8_t>(label_length);
            for (size_t i = 0; i < label_length; ++i) {
                out[offset++] = static_cast<uint8_t>(label[i]);
            }
            if (ch == '\0') {
                break;
            }
            label = p + 1;
            label_length = 0;
            continue;
        }
        ++label_length;
    }
    if (offset >= capacity) {
        return false;
    }
    out[offset++] = 0;
    return true;
}

inline bool build_query(const char* host,
                        uint16_t query_id,
                        uint8_t* out,
                        size_t capacity,
                        size_t& out_length) {
    out_length = 0;
    if (host == nullptr || *host == '\0' || out == nullptr || capacity < 17) {
        return false;
    }
    for (size_t i = 0; i < capacity; ++i) {
        out[i] = 0;
    }
    usernet::store_be16(out + 0, query_id);
    usernet::store_be16(out + 2, 0x0100u);
    usernet::store_be16(out + 4, 1);

    size_t offset = 12;
    if (!append_dns_name(out, capacity, offset, host) || offset + 4 > capacity) {
        return false;
    }
    usernet::store_be16(out + offset, 1);
    usernet::store_be16(out + offset + 2, 1);
    out_length = offset + 4;
    return true;
}

inline bool skip_name(const uint8_t* message,
                      size_t message_length,
                      size_t& offset) {
    if (message == nullptr || offset >= message_length) {
        return false;
    }
    while (offset < message_length) {
        uint8_t length = message[offset++];
        if (length == 0) {
            return true;
        }
        if ((length & 0xC0u) == 0xC0u) {
            if (offset >= message_length) {
                return false;
            }
            ++offset;
            return true;
        }
        if ((length & 0xC0u) != 0 || offset + length > message_length) {
            return false;
        }
        offset += length;
    }
    return false;
}

inline bool parse_response(const uint8_t* message,
                           size_t message_length,
                           uint16_t expected_query_id,
                           uint8_t out_ip[4]) {
    if (message == nullptr || out_ip == nullptr || message_length < 12) {
        return false;
    }
    if (usernet::load_be16(message + 0) != expected_query_id) {
        return false;
    }
    uint16_t flags = usernet::load_be16(message + 2);
    if ((flags & 0x8000u) == 0 || (flags & 0x000Fu) != 0) {
        return false;
    }

    uint16_t question_count = usernet::load_be16(message + 4);
    uint16_t answer_count = usernet::load_be16(message + 6);
    size_t offset = 12;

    for (uint16_t i = 0; i < question_count; ++i) {
        if (!skip_name(message, message_length, offset) || offset + 4 > message_length) {
            return false;
        }
        offset += 4;
    }

    for (uint16_t i = 0; i < answer_count; ++i) {
        if (!skip_name(message, message_length, offset) || offset + 10 > message_length) {
            return false;
        }
        uint16_t type = usernet::load_be16(message + offset);
        uint16_t klass = usernet::load_be16(message + offset + 2);
        uint16_t rdlength = usernet::load_be16(message + offset + 8);
        offset += 10;
        if (offset + rdlength > message_length) {
            return false;
        }
        if (type == 1 && klass == 1 && rdlength == 4) {
            for (size_t j = 0; j < 4; ++j) {
                out_ip[j] = message[offset + j];
            }
            return true;
        }
        offset += rdlength;
    }

    return false;
}

inline bool prepare_context(ResolverContext& ctx) {
    networkd_protocol::Registry* registry = nullptr;
    if (!open_registry(ctx.network_registry_handle, registry)) {
        g_last_status = ResolveStatus::NetworkRegistryUnavailable;
        return false;
    }
    for (size_t spins = 0;
         registry->magic != networkd_protocol::kRegistryMagic ||
         registry->version != networkd_protocol::kRegistryVersion ||
         registry->server_pipe_id == 0;
         ++spins) {
        if (spins >= kRegistryPollSpins) {
            close_context(ctx);
            g_last_status = ResolveStatus::NetworkRegistryTimeout;
            return false;
        }
        yield();
    }

    long device_handle = net_device_open(0);
    if (device_handle < 0) {
        close_context(ctx);
        g_last_status = ResolveStatus::NoIpv4Config;
        return false;
    }

    descriptor_defs::NetIpv4Config cfg{};
    bool have_config =
        net_device_get_ipv4_config(static_cast<uint32_t>(device_handle), &cfg) == 0 &&
        (cfg.flags & descriptor_defs::kNetIpv4FlagEnabled) != 0 &&
        !ipv4_is_zero(cfg.address);
    descriptor_close(static_cast<uint32_t>(device_handle));
    if (!have_config) {
        close_context(ctx);
        g_last_status = ResolveStatus::NoIpv4Config;
        return false;
    }

    for (size_t i = 0; i < 4; ++i) {
        ctx.source_ip[i] = cfg.address[i];
        ctx.dns_server[i] = !ipv4_is_zero(cfg.dns) ? cfg.dns[i] : cfg.gateway[i];
    }
    if (ipv4_is_zero(ctx.dns_server)) {
        close_context(ctx);
        g_last_status = ResolveStatus::NoDnsServer;
        return false;
    }

    uint64_t server_flags = static_cast<uint64_t>(descriptor_defs::Flag::Writable) |
                            static_cast<uint64_t>(descriptor_defs::Flag::Async);
    long server_pipe = pipe_open_existing(server_flags, registry->server_pipe_id);
    if (server_pipe < 0) {
        close_context(ctx);
        g_last_status = ResolveStatus::ServerPipeUnavailable;
        return false;
    }
    ctx.server_pipe = static_cast<uint32_t>(server_pipe);

    uint64_t reply_flags = static_cast<uint64_t>(descriptor_defs::Flag::Readable) |
                           static_cast<uint64_t>(descriptor_defs::Flag::Async);
    long reply_pipe = pipe_open_new(reply_flags);
    if (reply_pipe < 0) {
        close_context(ctx);
        g_last_status = ResolveStatus::ReplyPipeUnavailable;
        return false;
    }
    ctx.reply_pipe = static_cast<uint32_t>(reply_pipe);

    auto* reply_info = allocate_pipe_info_buffer();
    if (reply_info == nullptr) {
        close_context(ctx);
        return false;
    }
    bool have_reply_info = pipe_get_info(ctx.reply_pipe, reply_info) == 0 &&
                           reply_info->id != 0;
    uint32_t reply_pipe_id = reply_info->id;
    unmap(reply_info, sizeof(*reply_info));
    if (!have_reply_info) {
        close_context(ctx);
        g_last_status = ResolveStatus::ReplyPipeUnavailable;
        return false;
    }

    for (uint16_t i = 0; i < kEphemeralPortCount; ++i) {
        uint16_t candidate = static_cast<uint16_t>(kFirstEphemeralPort + i);
        if (!bind_udp(ctx.server_pipe, reply_pipe_id, candidate)) {
            close_context(ctx);
            g_last_status = ResolveStatus::BindFailed;
            return false;
        }
        int32_t bind_status = networkd_protocol::kStatusIo;
        if (!wait_for_bind_response(ctx.reply_pipe, candidate, bind_status)) {
            close_context(ctx);
            g_last_status = ResolveStatus::BindFailed;
            return false;
        }
        if (bind_status == networkd_protocol::kStatusOk) {
            ctx.source_port = candidate;
            return true;
        }
        if (bind_status != networkd_protocol::kStatusInUse) {
            close_context(ctx);
            g_last_status = ResolveStatus::BindFailed;
            return false;
        }
    }

    close_context(ctx);
    g_last_status = ResolveStatus::BindFailed;
    return false;
}

inline bool resolve_a(const char* host, uint8_t out_ip[4]) {
    g_last_status = ResolveStatus::Ok;
    ResolverContext ctx{};
    if (!prepare_context(ctx)) {
        return false;
    }

    for (size_t attempt = 0; attempt < kResolveAttempts; ++attempt) {
        uint8_t* query = allocate_query_buffer();
        if (query == nullptr) {
            close_context(ctx);
            return false;
        }
        size_t query_length = 0;
        uint16_t query_id = make_query_id(host);
        bool ok = build_query(host, query_id, query, kMaxDnsMessageSize, query_length) &&
                  send_udp(ctx.server_pipe,
                           ctx.source_port,
                           ctx.source_ip,
                           kDnsPort,
                           ctx.dns_server,
                           query,
                           query_length);
        unmap(query, kMaxDnsMessageSize);
        if (!ok) {
            close_context(ctx);
            g_last_status = ResolveStatus::QuerySendFailed;
            return false;
        }

        for (size_t spins = 0; spins < kReplyPollSpins; ++spins) {
            auto* message = allocate_message_buffer();
            if (message == nullptr) {
                close_context(ctx);
                return false;
            }
            if (!networkd_protocol::read_message(ctx.reply_pipe, *message)) {
                unmap(message, sizeof(*message));
                yield();
                continue;
            }
            if (message->type != networkd_protocol::kUdpPacketEvent ||
                message->udp_event.source_port != kDnsPort ||
                message->udp_event.destination_port != ctx.source_port) {
                unmap(message, sizeof(*message));
                continue;
            }
            if (parse_response(message->udp_event.payload,
                               message->udp_event.payload_length,
                               query_id,
                               out_ip)) {
                unmap(message, sizeof(*message));
                close_context(ctx);
                g_last_status = ResolveStatus::Ok;
                return true;
            }
            unmap(message, sizeof(*message));
        }

        for (size_t cooldown = 0; cooldown < 2000; ++cooldown) {
            yield();
        }
    }

    close_context(ctx);
    g_last_status = ResolveStatus::NoResponse;
    return false;
}

}  // namespace usernet::dns
