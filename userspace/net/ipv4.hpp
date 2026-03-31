#pragma once

#include <stddef.h>
#include <stdint.h>

#include "udp.hpp"

namespace usernet {

constexpr uint16_t kEtherTypeArp = 0x0806;
constexpr uint8_t kIpv4ProtocolIcmp = 1;
constexpr uint8_t kIcmpTypeEchoReply = 0;
constexpr uint8_t kIcmpTypeEchoRequest = 8;

struct ArpPacketView {
    uint16_t operation;
    uint8_t sender_mac[6];
    uint8_t sender_ip[4];
    uint8_t target_ip[4];
};

struct IcmpEchoReplyView {
    uint8_t source_ip[4];
    uint8_t destination_ip[4];
    uint8_t ttl;
    uint16_t identifier;
    uint16_t sequence;
    const uint8_t* payload;
    size_t payload_length;
};

inline uint32_t ipv4_to_u32(const uint8_t ip[4]) {
    return (static_cast<uint32_t>(ip[0]) << 24) |
           (static_cast<uint32_t>(ip[1]) << 16) |
           (static_cast<uint32_t>(ip[2]) << 8) |
           static_cast<uint32_t>(ip[3]);
}

inline void ipv4_from_u32(uint32_t value, uint8_t ip[4]) {
    ip[0] = static_cast<uint8_t>((value >> 24) & 0xFFu);
    ip[1] = static_cast<uint8_t>((value >> 16) & 0xFFu);
    ip[2] = static_cast<uint8_t>((value >> 8) & 0xFFu);
    ip[3] = static_cast<uint8_t>(value & 0xFFu);
}

inline bool ipv4_is_zero(const uint8_t ip[4]) {
    return ip[0] == 0 && ip[1] == 0 && ip[2] == 0 && ip[3] == 0;
}

inline bool ipv4_same_subnet(const uint8_t a[4],
                             const uint8_t b[4],
                             const uint8_t netmask[4]) {
    return (ipv4_to_u32(a) & ipv4_to_u32(netmask)) ==
           (ipv4_to_u32(b) & ipv4_to_u32(netmask));
}

inline bool parse_arp_frame(const uint8_t* frame,
                            size_t frame_length,
                            ArpPacketView& out) {
    if (frame == nullptr || frame_length < kEthernetHeaderSize + 28) {
        return false;
    }
    if (load_be16(frame + 12) != kEtherTypeArp) {
        return false;
    }
    const uint8_t* arp = frame + kEthernetHeaderSize;
    if (load_be16(arp + 0) != 0x0001 ||
        load_be16(arp + 2) != kEtherTypeIpv4 ||
        arp[4] != 6 ||
        arp[5] != 4) {
        return false;
    }
    out.operation = load_be16(arp + 6);
    for (size_t i = 0; i < 6; ++i) {
        out.sender_mac[i] = arp[8 + i];
    }
    for (size_t i = 0; i < 4; ++i) {
        out.sender_ip[i] = arp[14 + i];
        out.target_ip[i] = arp[24 + i];
    }
    return true;
}

inline bool build_arp_request_frame(uint8_t* out_frame,
                                    size_t out_capacity,
                                    size_t& out_length,
                                    const uint8_t source_mac[6],
                                    const uint8_t source_ip[4],
                                    const uint8_t target_ip[4]) {
    out_length = 0;
    if (out_frame == nullptr || out_capacity < kEthernetHeaderSize + 28) {
        return false;
    }
    static constexpr uint8_t kBroadcastMac[6] =
        {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    for (size_t i = 0; i < 6; ++i) {
        out_frame[i] = kBroadcastMac[i];
        out_frame[6 + i] = source_mac[i];
    }
    store_be16(out_frame + 12, kEtherTypeArp);
    uint8_t* arp = out_frame + kEthernetHeaderSize;
    store_be16(arp + 0, 0x0001);
    store_be16(arp + 2, kEtherTypeIpv4);
    arp[4] = 6;
    arp[5] = 4;
    store_be16(arp + 6, 0x0001);
    for (size_t i = 0; i < 6; ++i) {
        arp[8 + i] = source_mac[i];
        arp[18 + i] = 0;
    }
    for (size_t i = 0; i < 4; ++i) {
        arp[14 + i] = source_ip[i];
        arp[24 + i] = target_ip[i];
    }
    out_length = kEthernetHeaderSize + 28;
    return true;
}

inline bool parse_icmp_echo_reply_frame(const uint8_t* frame,
                                        size_t frame_length,
                                        IcmpEchoReplyView& out) {
    if (frame == nullptr ||
        frame_length < kEthernetHeaderSize + kIpv4HeaderMinSize + 8) {
        return false;
    }
    if (load_be16(frame + 12) != kEtherTypeIpv4) {
        return false;
    }
    const uint8_t* ipv4 = frame + kEthernetHeaderSize;
    if ((ipv4[0] >> 4) != 4) {
        return false;
    }
    uint8_t ihl = static_cast<uint8_t>((ipv4[0] & 0x0Fu) * 4u);
    if (ihl < kIpv4HeaderMinSize ||
        frame_length < kEthernetHeaderSize + ihl + 8 ||
        ipv4[9] != kIpv4ProtocolIcmp) {
        return false;
    }
    uint16_t total_length = load_be16(ipv4 + 2);
    if (total_length < ihl + 8 ||
        frame_length < kEthernetHeaderSize + total_length) {
        return false;
    }
    if (internet_checksum(ipv4, ihl) != 0) {
        return false;
    }
    const uint8_t* icmp = ipv4 + ihl;
    size_t icmp_length = total_length - ihl;
    if (icmp[0] != kIcmpTypeEchoReply || icmp[1] != 0 ||
        internet_checksum(icmp, icmp_length) != 0) {
        return false;
    }
    for (size_t i = 0; i < 4; ++i) {
        out.source_ip[i] = ipv4[12 + i];
        out.destination_ip[i] = ipv4[16 + i];
    }
    out.ttl = ipv4[8];
    out.identifier = load_be16(icmp + 4);
    out.sequence = load_be16(icmp + 6);
    out.payload = icmp + 8;
    out.payload_length = icmp_length - 8;
    return true;
}

inline bool build_icmp_echo_frame(uint8_t* out_frame,
                                  size_t out_capacity,
                                  size_t& out_length,
                                  const uint8_t source_mac[6],
                                  const uint8_t destination_mac[6],
                                  const uint8_t source_ip[4],
                                  const uint8_t destination_ip[4],
                                  uint16_t identifier,
                                  uint16_t sequence,
                                  const void* payload,
                                  size_t payload_length) {
    out_length = 0;
    if (out_frame == nullptr || payload == nullptr || payload_length > 1472 - 8) {
        return false;
    }
    size_t icmp_length = 8 + payload_length;
    size_t total_length = kEthernetHeaderSize + kIpv4HeaderMinSize + icmp_length;
    if (total_length > out_capacity) {
        return false;
    }
    for (size_t i = 0; i < 6; ++i) {
        out_frame[i] = destination_mac[i];
        out_frame[6 + i] = source_mac[i];
    }
    store_be16(out_frame + 12, kEtherTypeIpv4);
    uint8_t* ipv4 = out_frame + kEthernetHeaderSize;
    ipv4[0] = 0x45;
    ipv4[1] = 0;
    store_be16(ipv4 + 2, static_cast<uint16_t>(kIpv4HeaderMinSize + icmp_length));
    store_be16(ipv4 + 4, 0);
    store_be16(ipv4 + 6, 0);
    ipv4[8] = 64;
    ipv4[9] = kIpv4ProtocolIcmp;
    store_be16(ipv4 + 10, 0);
    for (size_t i = 0; i < 4; ++i) {
        ipv4[12 + i] = source_ip[i];
        ipv4[16 + i] = destination_ip[i];
    }
    store_be16(ipv4 + 10, internet_checksum(ipv4, kIpv4HeaderMinSize));

    uint8_t* icmp = ipv4 + kIpv4HeaderMinSize;
    icmp[0] = kIcmpTypeEchoRequest;
    icmp[1] = 0;
    store_be16(icmp + 2, 0);
    store_be16(icmp + 4, identifier);
    store_be16(icmp + 6, sequence);
    const uint8_t* payload_bytes = static_cast<const uint8_t*>(payload);
    for (size_t i = 0; i < payload_length; ++i) {
        icmp[8 + i] = payload_bytes[i];
    }
    store_be16(icmp + 2, internet_checksum(icmp, icmp_length));
    out_length = total_length;
    return true;
}

}  // namespace usernet
