#include <stddef.h>
#include <stdint.h>

#include "../crt/syscall.hpp"
#include "../net/network_protocol.hpp"
#include "../net/udp.hpp"

namespace {

constexpr uint16_t kDhcpClientPort = 68;
constexpr uint16_t kDhcpServerPort = 67;
constexpr uint8_t kDhcpDiscover = 1;
constexpr uint8_t kDhcpOffer = 2;
constexpr uint8_t kDhcpRequest = 3;
constexpr uint8_t kDhcpAck = 5;
constexpr uint8_t kDhcpMagicCookie[4] = {99, 130, 83, 99};
constexpr uint8_t kIpv4Zero[4] = {0, 0, 0, 0};
constexpr uint8_t kIpv4Broadcast[4] = {255, 255, 255, 255};
constexpr size_t kDhcpBaseSize = 240;
constexpr size_t kConfigBufferSize = 256;
constexpr size_t kConfigPathSize = 160;
constexpr size_t kDhcpPollSpins = 20000;
constexpr size_t kDhcpRetryBackoffYields = 50000;

networkd_protocol::Message* g_message_buffer = nullptr;

struct DhcpReply {
    bool valid;
    uint8_t message_type;
    uint8_t yiaddr[4];
    uint8_t server_id[4];
    uint8_t subnet_mask[4];
    uint8_t router[4];
    uint8_t dns[4];
};

uint32_t ipv4_to_u32(const uint8_t ip[4]) {
    return (static_cast<uint32_t>(ip[0]) << 24) |
           (static_cast<uint32_t>(ip[1]) << 16) |
           (static_cast<uint32_t>(ip[2]) << 8) |
           static_cast<uint32_t>(ip[3]);
}

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
    char buffer[16];
    size_t length = 0;
    if (value == 0) {
        buffer[length++] = '0';
    } else {
        while (value != 0 && length < sizeof(buffer)) {
            buffer[length++] = static_cast<char>('0' + (value % 10));
            value /= 10;
        }
    }
    for (size_t i = 0; i < length / 2; ++i) {
        char tmp = buffer[i];
        buffer[i] = buffer[length - 1 - i];
        buffer[length - 1 - i] = tmp;
    }
    buffer[length] = '\0';
    print(buffer);
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

networkd_protocol::Message* allocate_message_buffer() {
    if (g_message_buffer == nullptr) {
        g_message_buffer = static_cast<networkd_protocol::Message*>(
            map_anonymous(sizeof(networkd_protocol::Message), MAP_WRITE));
    }
    if (g_message_buffer != nullptr) {
        for (size_t i = 0; i < sizeof(*g_message_buffer); ++i) {
            reinterpret_cast<uint8_t*>(g_message_buffer)[i] = 0;
        }
    }
    return g_message_buffer;
}

int g_registry_fail_reason = 0;

void print_decimal(uint32_t value) {
    char buffer[16];
    size_t length = 0;
    if (value == 0) {
        buffer[length++] = '0';
    } else {
        while (value != 0 && length < sizeof(buffer)) {
            buffer[length++] = static_cast<char>('0' + (value % 10));
            value /= 10;
        }
    }
    for (size_t i = 0; i < length / 2; ++i) {
        char tmp = buffer[i];
        buffer[i] = buffer[length - 1 - i];
        buffer[length - 1 - i] = tmp;
    }
    buffer[length] = '\0';
    print(buffer);
}

void print_ipv4(const uint8_t ip[4]) {
    print_decimal(ip[0]);
    print(".");
    print_decimal(ip[1]);
    print(".");
    print_decimal(ip[2]);
    print(".");
    print_decimal(ip[3]);
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
        g_registry_fail_reason = 5;
        descriptor_close(static_cast<uint32_t>(shm));
        return false;
    }
    long info_result =
        shared_memory_get_info(static_cast<uint32_t>(shm), info);
    if (info_result != 0) {
        g_registry_fail_reason = 2;
        unmap(info, sizeof(*info));
        descriptor_close(static_cast<uint32_t>(shm));
        return false;
    }
    uint64_t info_base = info->base;
    uint64_t info_length = info->length;
    unmap(info, sizeof(*info));
    if (info_base == 0) {
        g_registry_fail_reason = 3;
        descriptor_close(static_cast<uint32_t>(shm));
        return false;
    }
    if (info_length < sizeof(networkd_protocol::Registry)) {
        g_registry_fail_reason = 4;
        descriptor_close(static_cast<uint32_t>(shm));
        return false;
    }
    handle = static_cast<uint32_t>(shm);
    registry = reinterpret_cast<networkd_protocol::Registry*>(info_base);
    return true;
}

char hex_digit(uint8_t value) {
    return (value < 10) ? static_cast<char>('0' + value)
                        : static_cast<char>('a' + (value - 10));
}

bool build_mount_subpath(const char* mount,
                         const char* suffix,
                         char* out,
                         size_t out_size) {
    if (mount == nullptr || mount[0] == '\0' || out == nullptr || out_size == 0) {
        return false;
    }
    size_t idx = 0;
    out[idx++] = '/';
    for (size_t i = 0; mount[i] != '\0'; ++i) {
        if (idx + 1 >= out_size) {
            return false;
        }
        out[idx++] = mount[i];
    }
    if (suffix != nullptr && suffix[0] != '\0') {
        if (idx + 1 >= out_size) {
            return false;
        }
        out[idx++] = '/';
        for (size_t i = 0; suffix[i] != '\0'; ++i) {
            if (idx + 1 >= out_size) {
                return false;
            }
            out[idx++] = suffix[i];
        }
    }
    out[idx] = '\0';
    return true;
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
    return true;
}

bool read_file_from_mounts(const char* suffix,
                           char* buffer,
                           size_t buffer_size,
                           size_t& out_len) {
    if (read_file_into_buffer(suffix, buffer, buffer_size, out_len)) {
        return true;
    }
    long dir = directory_open("/");
    if (dir < 0) {
        return false;
    }
    DirEntry entry{};
    char path[kConfigPathSize];
    while (directory_read(static_cast<uint32_t>(dir), &entry) > 0) {
        if (entry.name[0] == '\0') {
            continue;
        }
        if (!build_mount_subpath(entry.name, suffix, path, sizeof(path))) {
            continue;
        }
        if (read_file_into_buffer(path, buffer, buffer_size, out_len)) {
            directory_close(static_cast<uint32_t>(dir));
            return true;
        }
    }
    directory_close(static_cast<uint32_t>(dir));
    return false;
}

bool interface_uses_static_ipv4(const uint8_t mac[6]) {
    char path[kConfigPathSize];
    size_t idx = 0;
    const char prefix[] = "config/network/";
    while (prefix[idx] != '\0' && idx + 1 < sizeof(path)) {
        path[idx] = prefix[idx];
        ++idx;
    }
    for (size_t i = 0; i < 6; ++i) {
        if (idx + 3 >= sizeof(path)) {
            return false;
        }
        path[idx++] = hex_digit(static_cast<uint8_t>((mac[i] >> 4) & 0x0F));
        path[idx++] = hex_digit(static_cast<uint8_t>(mac[i] & 0x0F));
        if (i != 5) {
            path[idx++] = '-';
        }
    }
    const char suffix[] = "/interface.cfg";
    for (size_t i = 0; suffix[i] != '\0' && idx + 1 < sizeof(path); ++i) {
        path[idx++] = suffix[i];
    }
    path[idx] = '\0';

    char buffer[kConfigBufferSize];
    size_t len = 0;
    if (!read_file_from_mounts(path, buffer, sizeof(buffer), len)) {
        return false;
    }

    const char key[] = "IPV4.ADDRESS";
    for (size_t i = 0; i < len; ++i) {
        size_t j = 0;
        while (key[j] != '\0' && i + j < len && buffer[i + j] == key[j]) {
            ++j;
        }
        if (key[j] != '\0') {
            continue;
        }
        size_t pos = i + j;
        while (pos < len && (buffer[pos] == ' ' || buffer[pos] == '\t')) {
            ++pos;
        }
        if (pos < len && buffer[pos] == ':') {
            return true;
        }
    }
    return false;
}

bool bind_udp(uint32_t server_handle, uint32_t reply_pipe_id, uint16_t port) {
    auto* request = allocate_message_buffer();
    if (request == nullptr) {
        return false;
    }
    networkd_protocol::init_message(*request,
                                    networkd_protocol::kBindUdpRequest);
    request->bind_request.reply_pipe_id = reply_pipe_id;
    request->bind_request.port = port;
    return networkd_protocol::write_message(server_handle, *request);
}

bool send_udp(uint32_t server_handle,
              uint16_t source_port,
              const uint8_t source_ip[4],
              uint16_t destination_port,
              const uint8_t destination_ip[4],
              const void* payload,
              size_t payload_length,
              uint16_t flags) {
    if (payload_length > networkd_protocol::kMaxUdpPayload) {
        return false;
    }
    auto* request = allocate_message_buffer();
    if (request == nullptr) {
        return false;
    }
    networkd_protocol::init_message(*request,
                                    networkd_protocol::kSendUdpRequest);
    request->send_request.source_port = source_port;
    request->send_request.destination_port = destination_port;
    request->send_request.flags = flags;
    request->send_request.payload_length = static_cast<uint16_t>(payload_length);
    for (size_t i = 0; i < 4; ++i) {
        request->send_request.source_ip[i] = source_ip[i];
        request->send_request.destination_ip[i] = destination_ip[i];
    }
    const uint8_t* bytes = static_cast<const uint8_t*>(payload);
    for (size_t i = 0; i < payload_length; ++i) {
        request->send_request.payload[i] = bytes[i];
    }
    return networkd_protocol::write_message(server_handle, *request);
}

size_t append_option(uint8_t* options,
                     size_t offset,
                     uint8_t code,
                     const void* data,
                     uint8_t length) {
    options[offset++] = code;
    options[offset++] = length;
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    for (uint8_t i = 0; i < length; ++i) {
        options[offset++] = bytes[i];
    }
    return offset;
}

size_t build_dhcp_message(uint8_t* out,
                          size_t out_capacity,
                          uint32_t xid,
                          const uint8_t mac[6],
                          uint8_t type,
                          const uint8_t requested_ip[4],
                          const uint8_t server_id[4]) {
    if (out == nullptr || out_capacity < kDhcpBaseSize) {
        return 0;
    }

    for (size_t i = 0; i < out_capacity; ++i) {
        out[i] = 0;
    }
    out[0] = 1;
    out[1] = 1;
    out[2] = 6;
    out[3] = 0;
    usernet::store_be32(out + 4, xid);
    usernet::store_be16(out + 8, 0);
    usernet::store_be16(out + 10, 0x8000u);
    for (size_t i = 0; i < 6; ++i) {
        out[28 + i] = mac[i];
    }
    for (size_t i = 0; i < 4; ++i) {
        out[236 + i] = kDhcpMagicCookie[i];
    }

    size_t offset = kDhcpBaseSize;
    offset = append_option(out, offset, 53, &type, 1);
    if (type == kDhcpRequest && requested_ip != nullptr && server_id != nullptr) {
        offset = append_option(out, offset, 50, requested_ip, 4);
        offset = append_option(out, offset, 54, server_id, 4);
    }
    const uint8_t prl[] = {1, 3, 6, 15, 28, 51, 54};
    offset = append_option(out, offset, 55, prl, sizeof(prl));
    out[offset++] = 255;
    return offset;
}

bool parse_dhcp_reply(const networkd_protocol::UdpPacketEvent& event,
                      uint32_t xid,
                      const uint8_t mac[6],
                      DhcpReply& out) {
    out = {};
    if (event.source_port != kDhcpServerPort ||
        event.destination_port != kDhcpClientPort ||
        event.payload_length < kDhcpBaseSize) {
        return false;
    }

    const uint8_t* payload = event.payload;
    if (payload[0] != 2 || payload[1] != 1 || payload[2] != 6) {
        return false;
    }
    if (usernet::load_be32(payload + 4) != xid) {
        return false;
    }
    for (size_t i = 0; i < 6; ++i) {
        if (payload[28 + i] != mac[i]) {
            return false;
        }
    }
    if (payload[236] != kDhcpMagicCookie[0] ||
        payload[237] != kDhcpMagicCookie[1] ||
        payload[238] != kDhcpMagicCookie[2] ||
        payload[239] != kDhcpMagicCookie[3]) {
        return false;
    }

    for (size_t i = 0; i < 4; ++i) {
        out.yiaddr[i] = payload[16 + i];
    }

    size_t offset = kDhcpBaseSize;
    while (offset < event.payload_length) {
        uint8_t code = payload[offset++];
        if (code == 0) {
            continue;
        }
        if (code == 255) {
            break;
        }
        if (offset >= event.payload_length) {
            break;
        }
        uint8_t length = payload[offset++];
        if (offset + length > event.payload_length) {
            break;
        }
        if (code == 53 && length == 1) {
            out.message_type = payload[offset];
        } else if (code == 54 && length == 4) {
            for (size_t i = 0; i < 4; ++i) {
                out.server_id[i] = payload[offset + i];
            }
        } else if (code == 1 && length == 4) {
            for (size_t i = 0; i < 4; ++i) {
                out.subnet_mask[i] = payload[offset + i];
            }
        } else if (code == 3 && length >= 4) {
            for (size_t i = 0; i < 4; ++i) {
                out.router[i] = payload[offset + i];
            }
        } else if (code == 6 && length >= 4) {
            for (size_t i = 0; i < 4; ++i) {
                out.dns[i] = payload[offset + i];
            }
        }
        offset += length;
    }

    out.valid = out.message_type != 0 && out.server_id[0] + out.server_id[1] +
                                     out.server_id[2] + out.server_id[3] != 0;
    return out.valid;
}

bool wait_for_reply(uint32_t reply_handle,
                    uint32_t xid,
                    const uint8_t mac[6],
                    uint8_t expected_type,
                    DhcpReply& out_reply) {
    for (size_t spins = 0; spins < kDhcpPollSpins; ++spins) {
        auto* message = allocate_message_buffer();
        if (message == nullptr) {
            return false;
        }
        if (!networkd_protocol::read_message(reply_handle, *message)) {
            yield();
            continue;
        }
        if (message->type != networkd_protocol::kUdpPacketEvent) {
            continue;
        }
        if (!parse_dhcp_reply(message->udp_event, xid, mac, out_reply)) {
            continue;
        }
        if (out_reply.message_type == expected_type) {
            return true;
        }
    }
    return false;
}

uint32_t make_xid(const uint8_t mac[6]) {
    return 0x4E540000u |
           (static_cast<uint32_t>(mac[2]) << 16) |
           (static_cast<uint32_t>(mac[4]) << 8) |
           static_cast<uint32_t>(mac[5]);
}

bool attempt_dhcp(uint32_t server_pipe,
                  uint32_t reply_pipe,
                  usernet::Device& device,
                  networkd_protocol::Registry* registry) {
    uint32_t xid = make_xid(device.info.mac);
    if (registry != nullptr) {
        registry->dhcp_state = networkd_protocol::kStateDiscoverSent;
        ++registry->dhcp_attempts;
        registry->dhcp_last_error = networkd_protocol::kErrorNone;
    }

    uint8_t discover[300];
    size_t discover_length =
        build_dhcp_message(discover,
                           sizeof(discover),
                           xid,
                           device.info.mac,
                           kDhcpDiscover,
                           nullptr,
                           nullptr);
    if (discover_length == 0 ||
        !send_udp(server_pipe,
                  kDhcpClientPort,
                  kIpv4Zero,
                  kDhcpServerPort,
                  kIpv4Broadcast,
                  discover,
                  discover_length,
                  networkd_protocol::kSendFlagBroadcast)) {
        if (registry != nullptr) {
            registry->dhcp_state = networkd_protocol::kStateError;
            registry->dhcp_last_error = networkd_protocol::kErrorSendDiscover;
        }
        print_line("dhcp: failed to send discover");
        return false;
    }
    print_line("dhcp: discover sent");

    DhcpReply offer{};
    if (!wait_for_reply(reply_pipe,
                        xid,
                        device.info.mac,
                        kDhcpOffer,
                        offer)) {
        if (registry != nullptr) {
            registry->dhcp_state = networkd_protocol::kStateWaitingOffer;
            registry->dhcp_last_error = networkd_protocol::kErrorNoOffer;
        }
        print_line("dhcp: no offer received");
        return false;
    }
    if (registry != nullptr) {
        registry->dhcp_state = networkd_protocol::kStateOfferReceived;
        registry->dhcp_last_offer = ipv4_to_u32(offer.yiaddr);
    }
    print("dhcp: offer ");
    print_ipv4(offer.yiaddr);
    print(" from ");
    print_ipv4(offer.server_id);
    print("\n");

    uint8_t request[300];
    size_t request_length =
        build_dhcp_message(request,
                           sizeof(request),
                           xid,
                           device.info.mac,
                           kDhcpRequest,
                           offer.yiaddr,
                           offer.server_id);
    if (request_length == 0 ||
        !send_udp(server_pipe,
                  kDhcpClientPort,
                  kIpv4Zero,
                  kDhcpServerPort,
                  kIpv4Broadcast,
                  request,
                  request_length,
                  networkd_protocol::kSendFlagBroadcast)) {
        if (registry != nullptr) {
            registry->dhcp_state = networkd_protocol::kStateError;
            registry->dhcp_last_error = networkd_protocol::kErrorSendRequest;
        }
        print_line("dhcp: failed to send request");
        return false;
    }
    if (registry != nullptr) {
        registry->dhcp_state = networkd_protocol::kStateRequestSent;
    }
    print_line("dhcp: request sent");

    DhcpReply ack{};
    if (!wait_for_reply(reply_pipe,
                        xid,
                        device.info.mac,
                        kDhcpAck,
                        ack)) {
        if (registry != nullptr) {
            registry->dhcp_state = networkd_protocol::kStateError;
            registry->dhcp_last_error = networkd_protocol::kErrorNoAck;
        }
        print_line("dhcp: no ack received");
        return false;
    }
    if (registry != nullptr) {
        registry->dhcp_state = networkd_protocol::kStateAckReceived;
        registry->dhcp_last_ack = ipv4_to_u32(ack.yiaddr);
    }
    print("dhcp: ack ");
    print_ipv4(ack.yiaddr);
    print("\n");

    descriptor_defs::NetIpv4Config config{};
    for (size_t i = 0; i < 4; ++i) {
        config.address[i] = ack.yiaddr[i];
        config.netmask[i] = ack.subnet_mask[i];
        config.gateway[i] = ack.router[i];
        config.dns[i] = ack.dns[i];
    }
    if (config.netmask[0] == 0 && config.netmask[1] == 0 &&
        config.netmask[2] == 0 && config.netmask[3] == 0) {
        config.netmask[0] = 255;
        config.netmask[1] = 255;
        config.netmask[2] = 255;
        config.netmask[3] = 0;
    }
    config.flags = descriptor_defs::kNetIpv4FlagEnabled |
                   descriptor_defs::kNetIpv4FlagDhcp;
    if (net_device_set_ipv4_config(device.handle, &config) != 0) {
        if (registry != nullptr) {
            registry->dhcp_state = networkd_protocol::kStateError;
            registry->dhcp_last_error = networkd_protocol::kErrorApplyConfig;
        }
        return false;
    }
    if (registry != nullptr) {
        registry->dhcp_state = networkd_protocol::kStateLeaseApplied;
    }
    return true;
}

}  // namespace

int main(uint64_t, uint64_t) {
    usernet::Device device{};
    if (!usernet::open_device(device, 0)) {
        print_line("dhcp: failed to open net device 0");
        return 31;
    }

    if (interface_uses_static_ipv4(device.info.mac)) {
        print_line("dhcp: static IPv4 configured, skipping");
        return 0;
    }

    descriptor_defs::NetIpv4Config existing{};
    if (net_device_get_ipv4_config(device.handle, &existing) == 0 &&
        (existing.flags & descriptor_defs::kNetIpv4FlagEnabled) != 0 &&
        (existing.flags & descriptor_defs::kNetIpv4FlagDhcp) == 0) {
        print_line("dhcp: interface already has static IPv4, skipping");
        return 0;
    }

    uint32_t registry_handle = 0;
    networkd_protocol::Registry* registry = nullptr;
    if (!open_registry(registry_handle, registry)) {
        print_line("dhcp: failed to open network registry");
        return 320 + g_registry_fail_reason;
    }
    while (registry->magic != networkd_protocol::kRegistryMagic ||
           registry->version != networkd_protocol::kRegistryVersion ||
           registry->server_pipe_id == 0) {
        yield();
    }
    registry->dhcp_state = networkd_protocol::kStateStarting;
    registry->dhcp_attempts = 0;
    registry->dhcp_last_offer = 0;
    registry->dhcp_last_ack = 0;
    registry->dhcp_last_error = networkd_protocol::kErrorNone;

    uint64_t reply_flags = static_cast<uint64_t>(descriptor_defs::Flag::Readable) |
                           static_cast<uint64_t>(descriptor_defs::Flag::Async);
    long reply_pipe = pipe_open_new(reply_flags);
    if (reply_pipe < 0) {
        registry->dhcp_state = networkd_protocol::kStateError;
        registry->dhcp_last_error = networkd_protocol::kErrorCreateReplyPipe;
        print_line("dhcp: failed to create reply pipe");
        return 33;
    }
    auto* reply_info = allocate_pipe_info_buffer();
    if (reply_info == nullptr) {
        registry->dhcp_state = networkd_protocol::kStateError;
        registry->dhcp_last_error = networkd_protocol::kErrorReplyPipeInfo;
        print_line("dhcp: failed to allocate reply pipe info buffer");
        return 34;
    }
    long reply_info_result =
        pipe_get_info(static_cast<uint32_t>(reply_pipe), reply_info);
    uint32_t reply_pipe_id = reply_info->id;
    unmap(reply_info, sizeof(*reply_info));
    if (reply_info_result != 0 || reply_pipe_id == 0) {
        registry->dhcp_state = networkd_protocol::kStateError;
        registry->dhcp_last_error = networkd_protocol::kErrorReplyPipeInfo;
        print_line("dhcp: failed to query reply pipe");
        return 34;
    }
    registry->dhcp_state = networkd_protocol::kStateReplyPipeReady;
    print("dhcp: reply pipe id=");
    print_u32(reply_pipe_id);
    print("\n");

    uint64_t server_flags = static_cast<uint64_t>(descriptor_defs::Flag::Writable) |
                            static_cast<uint64_t>(descriptor_defs::Flag::Async);
    long server_pipe =
        pipe_open_existing(server_flags, registry->server_pipe_id);
    if (server_pipe < 0) {
        registry->dhcp_state = networkd_protocol::kStateError;
        registry->dhcp_last_error = networkd_protocol::kErrorOpenServerPipe;
        print_line("dhcp: failed to open networkd pipe");
        return 35;
    }
    registry->dhcp_state = networkd_protocol::kStateServerPipeOpen;
    print("dhcp: server pipe id=");
    print_u32(registry->server_pipe_id);
    print("\n");

    if (!bind_udp(static_cast<uint32_t>(server_pipe), reply_pipe_id, kDhcpClientPort)) {
        registry->dhcp_state = networkd_protocol::kStateError;
        registry->dhcp_last_error = networkd_protocol::kErrorBindSend;
        print_line("dhcp: failed to send bind request");
        return 36;
    }
    registry->dhcp_state = networkd_protocol::kStateBindSent;
    print_line("dhcp: bind request sent");

    auto* bind_response = allocate_message_buffer();
    if (bind_response == nullptr) {
        registry->dhcp_state = networkd_protocol::kStateError;
        registry->dhcp_last_error = networkd_protocol::kErrorBindResponse;
        print_line("dhcp: failed to allocate bind response buffer");
        return 37;
    }
    print_line("dhcp: waiting for bind response");
    while (!networkd_protocol::read_message(static_cast<uint32_t>(reply_pipe),
                                            *bind_response)) {
        yield();
    }
    if (bind_response->type != networkd_protocol::kBindUdpResponse ||
        bind_response->bind_response.status != networkd_protocol::kStatusOk) {
        registry->dhcp_state = networkd_protocol::kStateError;
        registry->dhcp_last_error = networkd_protocol::kErrorBindResponse;
        print_line("dhcp: bind failed");
        return 37;
    }
    registry->dhcp_state = networkd_protocol::kStateBound;

    bool waiting_for_link_reported = false;
    for (;;) {
        if (net_device_get_info(device.handle, &device.info) == 0 &&
            (device.info.flags & descriptor_defs::kNetDeviceFlagUp) != 0) {
            waiting_for_link_reported = false;
            if (attempt_dhcp(static_cast<uint32_t>(server_pipe),
                             static_cast<uint32_t>(reply_pipe),
                             device,
                             registry)) {
                print_line("dhcp: lease applied");
                return 0;
            }
            for (size_t i = 0; i < kDhcpRetryBackoffYields; ++i) {
                yield();
            }
        } else {
            registry->dhcp_state = networkd_protocol::kStateWaitingLink;
            if (!waiting_for_link_reported) {
                print_line("dhcp: waiting for link");
                waiting_for_link_reported = true;
            }
        }
        for (size_t i = 0; i < 200; ++i) {
            yield();
        }
    }
}
