#include <stddef.h>
#include <stdint.h>

#include "../crt/syscall.hpp"
#include "../net/network_protocol.hpp"
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
    print("usage: netctl info [index] | read [index] | set-ip <index> <ip> [mask] [gw] | status | debug [index]\n");
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

void print_ipv4_u32(uint32_t value) {
    uint8_t bytes[4] = {
        static_cast<uint8_t>((value >> 24) & 0xFFu),
        static_cast<uint8_t>((value >> 16) & 0xFFu),
        static_cast<uint8_t>((value >> 8) & 0xFFu),
        static_cast<uint8_t>(value & 0xFFu),
    };
    print_ipv4(bytes);
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
            print("\nrx queued: ");
            print_u32(info.rx_queued);
            print("\nflags: ");
            print_u32(info.flags);
            print("\n");
        }
        if (net_device_get_ipv4_config(static_cast<uint32_t>(handle), &cfg) == 0) {
            print("ipv4: ");
            print_ipv4(cfg.address);
            print("\nmask: ");
            print_ipv4(cfg.netmask);
            print("\ngateway: ");
            print_ipv4(cfg.gateway);
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
            return 1;
        }
        descriptor_defs::SharedMemoryInfo info{};
        if (shared_memory_get_info(static_cast<uint32_t>(handle), &info) != 0 ||
            info.base == 0 ||
            info.length < sizeof(networkd_protocol::Registry)) {
            print("netctl: network registry unavailable\n");
            return 1;
        }
        auto* registry =
            reinterpret_cast<networkd_protocol::Registry*>(info.base);
        print("networkd: ");
        print(state_name(registry->networkd_state));
        print("\ndhcp: ");
        print(state_name(registry->dhcp_state));
        print("\nattempts: ");
        print_u32(registry->dhcp_attempts);
        print("\nlast offer: ");
        print_ipv4_u32(registry->dhcp_last_offer);
        print("\nlast ack: ");
        print_ipv4_u32(registry->dhcp_last_ack);
        print("\nlast error: ");
        print_u32(registry->dhcp_last_error);
        print("\nrx frames: ");
        print_u32(registry->net_rx_frames);
        print("\nrx udp: ");
        print_u32(registry->net_rx_udp);
        print("\nrx delivered: ");
        print_u32(registry->net_rx_delivered);
        print("\ntx udp: ");
        print_u32(registry->net_tx_udp);
        print("\n");
        descriptor_close(static_cast<uint32_t>(handle));
        return 0;
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
