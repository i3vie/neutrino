#include <stddef.h>
#include <stdint.h>

#include "../crt/syscall.hpp"
#include "../net/network_protocol.hpp"
#include "../net/tcpd_protocol.hpp"
#include "descriptors.hpp"

namespace {

void print(const char* s) {
    static int32_t console = -1;
    if (console < 0) {
        console = static_cast<int32_t>(
            descriptor_open(static_cast<uint32_t>(descriptor_defs::Type::Console), 0));
        if (console < 0) {
            return;
        }
    }
    if (s == nullptr) {
        return;
    }
    size_t len = 0;
    while (s[len] != '\0') {
        ++len;
    }
    if (len != 0) {
        descriptor_write(static_cast<uint32_t>(console), s, len);
    }
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

char hex_digit(uint8_t value) {
    return (value < 10) ? static_cast<char>('0' + value)
                        : static_cast<char>('a' + (value - 10));
}

void print_mac(const uint8_t mac[6]) {
    char buf[18];
    for (size_t i = 0; i < 6; ++i) {
        buf[i * 3] = hex_digit(static_cast<uint8_t>((mac[i] >> 4) & 0x0F));
        buf[i * 3 + 1] = hex_digit(static_cast<uint8_t>(mac[i] & 0x0F));
        if (i != 5) {
            buf[i * 3 + 2] = ':';
        }
    }
    buf[17] = '\0';
    print(buf);
}

void print_ipv4(const uint8_t bytes[4]) {
    print_u32(bytes[0]);
    print(".");
    print_u32(bytes[1]);
    print(".");
    print_u32(bytes[2]);
    print(".");
    print_u32(bytes[3]);
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
    const char* start[6];
    size_t length[6];
    size_t count;
};

Tokens tokenize(const char* args) {
    Tokens out{};
    const char* cur = args;
    while (cur != nullptr && *cur != '\0' && out.count < 6) {
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

bool token_equals(const Tokens& tokens, size_t index, const char* text) {
    if (index >= tokens.count || text == nullptr) {
        return false;
    }
    size_t i = 0;
    while (text[i] != '\0' && i < tokens.length[index] &&
           tokens.start[index][i] == text[i]) {
        ++i;
    }
    return text[i] == '\0' && i == tokens.length[index];
}

void copy_token(const Tokens& tokens, size_t index, char* out, size_t out_size) {
    if (out == nullptr || out_size == 0) {
        return;
    }
    if (index >= tokens.count) {
        out[0] = '\0';
        return;
    }
    size_t n =
        (tokens.length[index] < out_size - 1) ? tokens.length[index] : out_size - 1;
    for (size_t i = 0; i < n; ++i) {
        out[i] = tokens.start[index][i];
    }
    out[n] = '\0';
}

long open_device_from_token(const Tokens& tokens, size_t index_token) {
    uint32_t index = 0;
    if (index_token < tokens.count) {
        char buf[16];
        copy_token(tokens, index_token, buf, sizeof(buf));
        if (!parse_u32(buf, index)) {
            return -1;
        }
    }
    return net_device_open(index);
}

void print_usage() {
    print("usage: netctl info [index] | status | debug [index] | read [index] | set-ip <index> <ip> [mask] [gw] [dns]\n");
}

void append_char(char* buffer, size_t buffer_size, size_t& length, char ch) {
    if (buffer == nullptr || buffer_size == 0 || length + 1 >= buffer_size) {
        return;
    }
    buffer[length++] = ch;
    buffer[length] = '\0';
}

void append_text(char* buffer, size_t buffer_size, size_t& length, const char* text) {
    if (buffer == nullptr || buffer_size == 0 || text == nullptr) {
        return;
    }
    while (*text != '\0' && length + 1 < buffer_size) {
        buffer[length++] = *text++;
    }
    buffer[length] = '\0';
}

void append_u32(char* buffer, size_t buffer_size, size_t& length, uint32_t value) {
    char tmp[16];
    size_t pos = 0;
    if (value == 0) {
        tmp[pos++] = '0';
    } else {
        char rev[16];
        size_t count = 0;
        while (value != 0 && count < sizeof(rev)) {
            rev[count++] = static_cast<char>('0' + (value % 10));
            value /= 10;
        }
        while (count != 0) {
            tmp[pos++] = rev[--count];
        }
    }
    tmp[pos] = '\0';
    append_text(buffer, buffer_size, length, tmp);
}

void append_ipv4(char* buffer, size_t buffer_size, size_t& length, uint32_t value) {
    uint8_t bytes[4] = {
        static_cast<uint8_t>((value >> 24) & 0xFFu),
        static_cast<uint8_t>((value >> 16) & 0xFFu),
        static_cast<uint8_t>((value >> 8) & 0xFFu),
        static_cast<uint8_t>(value & 0xFFu),
    };
    append_u32(buffer, buffer_size, length, bytes[0]);
    append_char(buffer, buffer_size, length, '.');
    append_u32(buffer, buffer_size, length, bytes[1]);
    append_char(buffer, buffer_size, length, '.');
    append_u32(buffer, buffer_size, length, bytes[2]);
    append_char(buffer, buffer_size, length, '.');
    append_u32(buffer, buffer_size, length, bytes[3]);
}

void append_hex64(char* buffer, size_t buffer_size, size_t& length, uint64_t value) {
    append_text(buffer, buffer_size, length, "0x");
    for (int i = 15; i >= 0; --i) {
        uint8_t nibble = static_cast<uint8_t>((value >> (i * 4)) & 0xFu);
        append_char(buffer,
                    buffer_size,
                    length,
                    static_cast<char>(nibble < 10 ? ('0' + nibble)
                                                  : ('a' + (nibble - 10))));
    }
}

const char* state_name(uint32_t state) {
    switch (state) {
        case networkd_protocol::kStateIdle: return "idle";
        case networkd_protocol::kStateStarting: return "starting";
        case networkd_protocol::kStateReady: return "ready";
        case networkd_protocol::kStateWaitingLink: return "waiting-link";
        case networkd_protocol::kStateBound: return "bound";
        case networkd_protocol::kStateReplyPipeReady: return "reply-pipe-ready";
        case networkd_protocol::kStateServerPipeOpen: return "server-pipe-open";
        case networkd_protocol::kStateBindSent: return "bind-sent";
        case networkd_protocol::kStateBindSeen: return "bind-seen";
        case networkd_protocol::kStateBindReplySent: return "bind-reply-sent";
        case networkd_protocol::kStateDiscoverSent: return "discover-sent";
        case networkd_protocol::kStateOfferReceived: return "offer-received";
        case networkd_protocol::kStateRequestSent: return "request-sent";
        case networkd_protocol::kStateAckReceived: return "ack-received";
        case networkd_protocol::kStateLeaseApplied: return "lease-applied";
        case networkd_protocol::kStateWaitingOffer: return "waiting-offer";
        case networkd_protocol::kStateError: return "error";
        default: return "unknown";
    }
}

const char* dhcp_error_name(uint32_t error) {
    switch (error) {
        case networkd_protocol::kErrorNone: return "none";
        case networkd_protocol::kErrorOpenDevice: return "open-device";
        case networkd_protocol::kErrorOpenRegistry: return "open-registry";
        case networkd_protocol::kErrorCreateReplyPipe: return "create-reply-pipe";
        case networkd_protocol::kErrorReplyPipeInfo: return "reply-pipe-info";
        case networkd_protocol::kErrorOpenServerPipe: return "open-networkd-pipe";
        case networkd_protocol::kErrorBindSend: return "send-bind";
        case networkd_protocol::kErrorBindResponse: return "bind-response";
        case networkd_protocol::kErrorSendDiscover: return "send-discover";
        case networkd_protocol::kErrorNoOffer: return "no-offer";
        case networkd_protocol::kErrorSendRequest: return "send-request";
        case networkd_protocol::kErrorNoAck: return "no-ack";
        case networkd_protocol::kErrorApplyConfig: return "apply-config";
        default: return "unknown";
    }
}

const char* tcpd_state_name(uint32_t state) {
    switch (state) {
        case tcpd_protocol::kStateIdle: return "idle";
        case tcpd_protocol::kStateStarting: return "starting";
        case tcpd_protocol::kStateReady: return "ready";
        case tcpd_protocol::kStateError: return "error";
        default: return "unknown";
    }
}

bool ipv4_is_zero(const uint8_t address[4]) {
    return address[0] == 0 && address[1] == 0 &&
           address[2] == 0 && address[3] == 0;
}

void print_health_summary(const networkd_protocol::Registry& registry) {
    print("health:\n");
    long handle = net_device_open(0);
    if (handle < 0) {
        print("[FAIL] no network interface 0; driver did not register a device\n");
        return;
    }

    descriptor_defs::NetDeviceInfo info{};
    if (net_device_get_info(static_cast<uint32_t>(handle), &info) != 0) {
        print("[FAIL] interface exists but device information is unavailable\n");
        descriptor_close(static_cast<uint32_t>(handle));
        return;
    }
    if ((info.flags & descriptor_defs::kNetDeviceFlagUp) == 0) {
        print("[FAIL] no carrier: cable unplugged, switch port down, or PHY negotiation failed\n");
    } else {
        print("[OK] carrier/link is up\n");
    }

    descriptor_defs::NetIpv4Config cfg{};
    bool have_cfg = net_device_get_ipv4_config(static_cast<uint32_t>(handle), &cfg) == 0;
    if (!have_cfg) {
        print("[FAIL] driver did not return IPv4 configuration\n");
    } else if ((cfg.flags & descriptor_defs::kNetIpv4FlagEnabled) == 0 ||
               ipv4_is_zero(cfg.address)) {
        if (registry.dhcp_state == networkd_protocol::kStateWaitingLink) {
            print("[WAIT] DHCP is waiting for carrier\n");
        } else if (registry.dhcp_last_error != networkd_protocol::kErrorNone) {
            print("[FAIL] no IPv4 lease; DHCP failed at ");
            print(dhcp_error_name(registry.dhcp_last_error));
            print("\n");
        } else {
            print("[WAIT] no IPv4 address; DHCP has not completed\n");
        }
    } else {
        print("[OK] IPv4 address is configured\n");
        if (ipv4_is_zero(cfg.gateway)) {
            print("[WARN] no default gateway; only the local subnet is reachable\n");
        } else {
            print("[OK] default gateway is configured\n");
        }
        if (ipv4_is_zero(cfg.dns)) {
            print("[WARN] no DNS server; names will fail but numeric IPs may work\n");
        } else {
            print("[OK] DNS server is configured\n");
        }
    }

    descriptor_defs::NetDeviceDebug debug{};
    if (descriptor_get_property(static_cast<uint32_t>(handle),
                                static_cast<uint32_t>(descriptor_defs::Property::NetDeviceDebug),
                                &debug,
                                sizeof(debug)) == 0) {
        uint32_t outstanding = debug.tx_submitted - debug.tx_completed;
        if (outstanding >= 31) {
            print("[FAIL] NIC TX ring appears full or stalled\n");
        } else if (outstanding != 0) {
            print("[WAIT] NIC has outstanding TX descriptors: ");
            print_u32(outstanding);
            print("\n");
        } else {
            print("[OK] NIC TX completions are caught up\n");
        }
        if (debug.rx_frames_dropped != 0) {
            print("[WARN] kernel RX queue dropped frames: ");
            print_u32(debug.rx_frames_dropped);
            print("\n");
        }
    } else {
        print("[WARN] driver-specific NIC diagnostics unavailable\n");
    }

    if (registry.net_tx_failures != 0) {
        print("[FAIL] networkd device-write failures: ");
        print_u32(registry.net_tx_failures);
        print("\n");
    }
    if (registry.arp_timeouts != 0) {
        print("[FAIL] ARP resolution timeouts: ");
        print_u32(registry.arp_timeouts);
        print(" (gateway/peer did not answer)\n");
    }
    if (registry.net_rx_frames == 0 && registry.net_tx_frames != 0) {
        print("[WARN] frames were transmitted but networkd has received none\n");
    }
    descriptor_close(static_cast<uint32_t>(handle));
}

bool print_tcpd_status() {
    long handle = shared_memory_open(tcpd_protocol::kRegistryName,
                                     sizeof(tcpd_protocol::Registry));
    if (handle < 0) {
        print("tcpd: registry not found\n");
        return false;
    }
    auto* info = allocate_shm_info_buffer();
    if (info == nullptr) {
        print("tcpd: failed to allocate shm info buffer\n");
        descriptor_close(static_cast<uint32_t>(handle));
        return false;
    }
    long result = shared_memory_get_info(static_cast<uint32_t>(handle), info);
    uint64_t base = info->base;
    uint64_t length = info->length;
    unmap(info, sizeof(*info));
    if (result != 0 || base == 0 || length < sizeof(tcpd_protocol::Registry)) {
        print("tcpd: registry unavailable or incompatible\n");
        descriptor_close(static_cast<uint32_t>(handle));
        return false;
    }

    auto* registry = reinterpret_cast<tcpd_protocol::Registry*>(base);
    if (registry->magic != tcpd_protocol::kRegistryMagic ||
        registry->version != tcpd_protocol::kRegistryVersion) {
        print("tcpd: registry has invalid magic or version\n");
        descriptor_close(static_cast<uint32_t>(handle));
        return false;
    }

    print("tcpd: "); print(tcpd_state_name(registry->state));
    print("\nlisteners: "); print_u32(registry->listeners);
    print("\nconnections: "); print_u32(registry->connections);
    print("\nrx segments: "); print_u32(registry->rx_segments);
    print("\ntx segments: "); print_u32(registry->tx_segments);
    print("\ninbound syns: "); print_u32(registry->inbound_syns);
    print("\nsyn-ack retransmits: "); print_u32(registry->syn_ack_retransmits);
    print("\nestablished total: "); print_u32(registry->established);
    print("\nack mismatches: "); print_u32(registry->wait_ack_mismatch);
    print("\nremote resets: "); print_u32(registry->remote_resets);
    print("\nremote fins: "); print_u32(registry->remote_fins);
    print("\nlast flags: "); print_u32(registry->last_flags);
    print("\nlast seq: "); print_u32(registry->last_seq);
    print("\nlast ack: "); print_u32(registry->last_ack);
    print("\nexpected seq: "); print_u32(registry->expected_seq);
    print("\nexpected ack: "); print_u32(registry->expected_ack);
    print("\noutbound syns: "); print_u32(registry->outbound_syns);
    print("\noutbound syn retries: ");
    print_u32(registry->outbound_syn_retransmits);
    print("\noutbound connect timeouts: ");
    print_u32(registry->outbound_connect_timeouts);
    print("\nconnections awaiting syn-ack: "); print_u32(registry->syn_sent);
    print("\n");
    if (registry->state != tcpd_protocol::kStateReady) {
        print("[FAIL] tcpd is not ready\n");
    } else if (registry->connections != 0 &&
               registry->tx_segments != 0 && registry->rx_segments == 0) {
        print("[WAIT] TCP transmitted but has received no segments\n");
    }
    if (registry->remote_resets != 0) {
        print("[WARN] remote TCP resets observed\n");
    }
    if (registry->wait_ack_mismatch != 0) {
        print("[WARN] TCP handshake ACK/sequence mismatches observed\n");
    }
    if (registry->outbound_syn_retransmits != 0) {
        print("[WARN] outbound SYN loss or delayed SYN-ACK observed\n");
    }
    if (registry->outbound_connect_timeouts != 0) {
        print("[FAIL] outbound TCP handshake timed out after retransmissions\n");
    } else if (registry->syn_sent != 0) {
        print("[WAIT] outbound TCP handshake is awaiting SYN-ACK\n");
    }
    descriptor_close(static_cast<uint32_t>(handle));
    return true;
}

}  // namespace

extern "C" int main(uint64_t arg_ptr, uint64_t /*flags*/) {
    const char* args = reinterpret_cast<const char*>(arg_ptr);
    Tokens tokens = tokenize(args);
    if (tokens.count == 0) {
        print_usage();
        return 1;
    }

    if (token_equals(tokens, 0, "info")) {
        long handle = open_device_from_token(tokens, 1);
        if (handle < 0) {
            print("netctl: unable to open net device\n");
            return 1;
        }

        char name[64];
        if (descriptor_get_property(static_cast<uint32_t>(handle),
                                    static_cast<uint32_t>(descriptor_defs::Property::CommonName),
                                    name,
                                    sizeof(name)) == 0) {
            print("name: ");
            print(name);
            print("\n");
        }

        descriptor_defs::NetDeviceInfo info{};
        descriptor_defs::NetIpv4Config cfg{};
        if (net_device_get_info(static_cast<uint32_t>(handle), &info) == 0) {
            print("index: ");
            print_u32(info.index);
            print("\nmac: ");
            print_mac(info.mac);
            print("\nlink: ");
            print((info.flags & descriptor_defs::kNetDeviceFlagUp) != 0
                      ? "up" : "down");
            print("\nmtu: ");
            print_u32(info.mtu);
            print("\nrx queued: ");
            print_u32(info.rx_queued);
            print("\nflags: ");
            print_u32(info.flags);
            print("\n");
        }
        if (net_device_get_ipv4_config(static_cast<uint32_t>(handle), &cfg) == 0) {
            print("ipv4 mode: ");
            if ((cfg.flags & descriptor_defs::kNetIpv4FlagEnabled) == 0) {
                print("disabled");
            } else if ((cfg.flags & descriptor_defs::kNetIpv4FlagDhcp) != 0) {
                print("dhcp");
            } else {
                print("static");
            }
            print("\n");
            print("ipv4: ");
            print_ipv4(cfg.address);
            print("\nmask: ");
            print_ipv4(cfg.netmask);
            print("\ngateway: ");
            print_ipv4(cfg.gateway);
            print("\ndns: ");
            print_ipv4(cfg.dns);
            print("\n");
        }
        descriptor_close(static_cast<uint32_t>(handle));
        return 0;
    }

    if (token_equals(tokens, 0, "set-ip")) {
        if (tokens.count < 3) {
            print_usage();
            return 1;
        }
        long handle = open_device_from_token(tokens, 1);
        if (handle < 0) {
            print("netctl: unable to open net device\n");
            return 1;
        }
        descriptor_defs::NetIpv4Config cfg{};
        cfg.flags = descriptor_defs::kNetIpv4FlagEnabled;
        char ip_buf[32];
        copy_token(tokens, 2, ip_buf, sizeof(ip_buf));
        if (!parse_ipv4(ip_buf, cfg.address)) {
            print("netctl: invalid ip\n");
            return 1;
        }
        if (tokens.count >= 4) {
            char mask_buf[32];
            copy_token(tokens, 3, mask_buf, sizeof(mask_buf));
            if (!parse_ipv4(mask_buf, cfg.netmask)) {
                print("netctl: invalid mask\n");
                return 1;
            }
        } else {
            cfg.netmask[0] = 255;
            cfg.netmask[1] = 255;
            cfg.netmask[2] = 255;
            cfg.netmask[3] = 0;
        }
        if (tokens.count >= 5) {
            char gw_buf[32];
            copy_token(tokens, 4, gw_buf, sizeof(gw_buf));
            if (!parse_ipv4(gw_buf, cfg.gateway)) {
                print("netctl: invalid gateway\n");
                return 1;
            }
        }
        if (tokens.count >= 6) {
            char dns_buf[32];
            copy_token(tokens, 5, dns_buf, sizeof(dns_buf));
            if (!parse_ipv4(dns_buf, cfg.dns)) {
                print("netctl: invalid dns\n");
                return 1;
            }
        }
        if (net_device_set_ipv4_config(static_cast<uint32_t>(handle), &cfg) != 0) {
            print("netctl: set-ip failed\n");
            return 1;
        }
        print("netctl: ipv4 updated\n");
        descriptor_close(static_cast<uint32_t>(handle));
        return 0;
    }

    if (token_equals(tokens, 0, "status")) {
        long handle =
            shared_memory_open(networkd_protocol::kRegistryName,
                               sizeof(networkd_protocol::Registry));
        if (handle < 0) {
            print("netctl: network registry not found\n");
            networkd_protocol::Registry unavailable{};
            print_health_summary(unavailable);
            return 1;
        }
        auto* info = allocate_shm_info_buffer();
        if (info == nullptr) {
            print("netctl: failed to allocate shm info buffer\n");
            networkd_protocol::Registry unavailable{};
            print_health_summary(unavailable);
            return 1;
        }
        long info_result =
            shared_memory_get_info(static_cast<uint32_t>(handle), info);
        uint64_t info_base = info->base;
        uint64_t info_length = info->length;
        unmap(info, sizeof(*info));
        if (info_result != 0 ||
            info_base == 0 ||
            info_length < sizeof(networkd_protocol::Registry)) {
            char buffer[192];
            size_t length = 0;
            buffer[0] = '\0';
            append_text(buffer, sizeof(buffer), length, "netctl: network registry unavailable");
            append_text(buffer, sizeof(buffer), length, " result=");
            append_u32(buffer, sizeof(buffer), length, static_cast<uint32_t>(info_result));
            append_text(buffer, sizeof(buffer), length, " base=");
            append_hex64(buffer, sizeof(buffer), length, info_base);
            append_text(buffer, sizeof(buffer), length, " length=");
            append_u32(buffer, sizeof(buffer), length, static_cast<uint32_t>(info_length));
            append_char(buffer, sizeof(buffer), length, '\n');
            print(buffer);
            networkd_protocol::Registry unavailable{};
            print_health_summary(unavailable);
            return 1;
        }
        auto* registry =
            reinterpret_cast<networkd_protocol::Registry*>(info_base);
        if (registry->magic != networkd_protocol::kRegistryMagic ||
            registry->version != networkd_protocol::kRegistryVersion) {
            print("networkd: registry is not initialized or has an incompatible version\n");
            networkd_protocol::Registry unavailable{};
            print_health_summary(unavailable);
            descriptor_close(static_cast<uint32_t>(handle));
            return 1;
        }
        char buffer[512];
        size_t length = 0;
        buffer[0] = '\0';
        append_text(buffer, sizeof(buffer), length, "networkd: ");
        append_text(buffer, sizeof(buffer), length, state_name(registry->networkd_state));
        append_text(buffer, sizeof(buffer), length, "\ndhcp: ");
        append_text(buffer, sizeof(buffer), length, state_name(registry->dhcp_state));
        append_text(buffer, sizeof(buffer), length, "\nattempts: ");
        append_u32(buffer, sizeof(buffer), length, registry->dhcp_attempts);
        append_text(buffer, sizeof(buffer), length, "\nlast offer: ");
        append_ipv4(buffer, sizeof(buffer), length, registry->dhcp_last_offer);
        append_text(buffer, sizeof(buffer), length, "\nlast ack: ");
        append_ipv4(buffer, sizeof(buffer), length, registry->dhcp_last_ack);
        append_text(buffer, sizeof(buffer), length, "\nlast error: ");
        append_u32(buffer, sizeof(buffer), length, registry->dhcp_last_error);
        append_text(buffer, sizeof(buffer), length, " (");
        append_text(buffer,
                    sizeof(buffer),
                    length,
                    dhcp_error_name(registry->dhcp_last_error));
        append_char(buffer, sizeof(buffer), length, ')');
        append_text(buffer, sizeof(buffer), length, "\nrx frames: ");
        append_u32(buffer, sizeof(buffer), length, registry->net_rx_frames);
        append_text(buffer, sizeof(buffer), length, "\nrx udp: ");
        append_u32(buffer, sizeof(buffer), length, registry->net_rx_udp);
        append_text(buffer, sizeof(buffer), length, "\nrx tcp: ");
        append_u32(buffer, sizeof(buffer), length, registry->net_rx_tcp);
        append_text(buffer, sizeof(buffer), length, "\nrx delivered: ");
        append_u32(buffer, sizeof(buffer), length, registry->net_rx_delivered);
        append_text(buffer, sizeof(buffer), length, "\ntx udp: ");
        append_u32(buffer, sizeof(buffer), length, registry->net_tx_udp);
        append_text(buffer, sizeof(buffer), length, "\ntx tcp: ");
        append_u32(buffer, sizeof(buffer), length, registry->net_tx_tcp);
        append_char(buffer, sizeof(buffer), length, '\n');
        print(buffer);
        print("rx arp: "); print_u32(registry->net_rx_arp);
        print("\nrx icmp: "); print_u32(registry->net_rx_icmp);
        print("\nrx unrecognized: "); print_u32(registry->net_rx_unrecognized);
        print("\nrx no binding: "); print_u32(registry->net_rx_no_binding);
        print("\ntx frames: "); print_u32(registry->net_tx_frames);
        print("\ntx failures: "); print_u32(registry->net_tx_failures);
        print("\narp requests: "); print_u32(registry->arp_requests);
        print("\narp cache hits: "); print_u32(registry->arp_cache_hits);
        print("\narp timeouts: "); print_u32(registry->arp_timeouts);
        print("\ninvalid control messages: "); print_u32(registry->control_invalid);
        print("\ndhcp discovers sent: "); print_u32(registry->dhcp_discovers_sent);
        print("\ndhcp requests sent: "); print_u32(registry->dhcp_requests_sent);
        print("\ndhcp replies seen: "); print_u32(registry->dhcp_replies_seen);
        print("\ndhcp replies rejected: "); print_u32(registry->dhcp_replies_rejected);
        print("\ndhcp offer timeouts: "); print_u32(registry->dhcp_offer_timeouts);
        print("\ndhcp ack timeouts: "); print_u32(registry->dhcp_ack_timeouts);
        print("\ndhcp last server: ");
        char server_buffer[32];
        size_t server_length = 0;
        server_buffer[0] = '\0';
        append_ipv4(server_buffer,
                    sizeof(server_buffer),
                    server_length,
                    registry->dhcp_last_server);
        append_char(server_buffer, sizeof(server_buffer), server_length, '\n');
        print(server_buffer);
        print_health_summary(*registry);
        descriptor_close(static_cast<uint32_t>(handle));
        print("---\n");
        return print_tcpd_status() ? 0 : 1;
    }

    if (token_equals(tokens, 0, "debug")) {
        long handle = open_device_from_token(tokens, 1);
        if (handle < 0) {
            print("netctl: unable to open net device\n");
            return 1;
        }
        descriptor_defs::NetDeviceDebug debug{};
        if (descriptor_get_property(static_cast<uint32_t>(handle),
                                    static_cast<uint32_t>(descriptor_defs::Property::NetDeviceDebug),
                                    &debug,
                                    sizeof(debug)) != 0) {
            print("netctl: debug unavailable\n");
            return 1;
        }
        print("status: ");
        print_u32(debug.status);
        print("\nrctl: ");
        print_u32(debug.rctl);
        print("\ntctl: ");
        print_u32(debug.tctl);
        print("\nrdh: ");
        print_u32(debug.rdh);
        print("\nrdt: ");
        print_u32(debug.rdt);
        print("\ntdh: ");
        print_u32(debug.tdh);
        print("\ntdt: ");
        print_u32(debug.tdt);
        print("\ntx submitted: ");
        print_u32(debug.tx_submitted);
        print("\ntx completed: ");
        print_u32(debug.tx_completed);
        print("\nrx desc seen: ");
        print_u32(debug.rx_desc_seen);
        print("\nrx frames passed: ");
        print_u32(debug.rx_frames_passed);
        print("\nrx queued: ");
        print_u32(debug.rx_queued);
        print("\nrx received: ");
        print_u32(debug.rx_frames_received);
        print("\nrx dropped: ");
        print_u32(debug.rx_frames_dropped);
        print("\n");
        descriptor_close(static_cast<uint32_t>(handle));
        return 0;
    }

    if (token_equals(tokens, 0, "read")) {
        long handle = open_device_from_token(tokens, 1);
        if (handle < 0) {
            print("netctl: unable to open net device\n");
            return 1;
        }
        uint8_t frame[1600];
        while (true) {
            long result = descriptor_read(static_cast<uint32_t>(handle),
                                          frame,
                                          sizeof(frame));
            if (result == kDescriptorWouldBlock) {
                yield();
                continue;
            }
            if (result < 0) {
                print("netctl: read failed\n");
                break;
            }
            if (result >= 14) {
                uint16_t ether_type =
                    static_cast<uint16_t>((static_cast<uint16_t>(frame[12]) << 8) |
                                          static_cast<uint16_t>(frame[13]));
                print("frame len=");
                print_u32(static_cast<uint32_t>(result));
                print(" ethertype=0x");
                char hex[5];
                hex[0] = hex_digit(static_cast<uint8_t>((ether_type >> 12) & 0x0F));
                hex[1] = hex_digit(static_cast<uint8_t>((ether_type >> 8) & 0x0F));
                hex[2] = hex_digit(static_cast<uint8_t>((ether_type >> 4) & 0x0F));
                hex[3] = hex_digit(static_cast<uint8_t>(ether_type & 0x0F));
                hex[4] = '\0';
                print(hex);
                print("\n");
            }
        }
        descriptor_close(static_cast<uint32_t>(handle));
        return 0;
    }

    print_usage();
    return 1;
}
