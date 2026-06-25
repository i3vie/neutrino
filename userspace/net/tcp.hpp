#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "udp.hpp"

namespace usernet {

constexpr uint8_t kIpv4ProtocolTcp = 6;
constexpr size_t kTcpHeaderMinSize = 20;
constexpr size_t kTcpMaxOptionBytes = 40;
constexpr size_t kTcpMaxPayload = 1460;

enum TcpFlags : uint16_t {
    kTcpFlagFin = 0x0001,
    kTcpFlagSyn = 0x0002,
    kTcpFlagRst = 0x0004,
    kTcpFlagPsh = 0x0008,
    kTcpFlagAck = 0x0010,
    kTcpFlagUrg = 0x0020,
};

struct TcpSegmentView {
    const uint8_t* options;
    size_t options_length;
    const uint8_t* payload;
    size_t payload_length;
    uint8_t source_ip[4];
    uint8_t destination_ip[4];
    uint16_t source_port;
    uint16_t destination_port;
    uint32_t sequence_number;
    uint32_t acknowledgment_number;
    uint16_t flags;
    uint16_t window_size;
};

inline uint32_t checksum_partial(const uint8_t* data, size_t length) {
    uint32_t sum = 0;
    while (length > 1) {
        sum += static_cast<uint32_t>((static_cast<uint16_t>(data[0]) << 8) |
                                     static_cast<uint16_t>(data[1]));
        data += 2;
        length -= 2;
    }
    if (length != 0) {
        sum += static_cast<uint32_t>(static_cast<uint16_t>(data[0]) << 8);
    }
    return sum;
}

inline uint16_t finish_checksum(uint32_t sum) {
    while ((sum >> 16) != 0) {
        sum = (sum & 0xFFFFu) + (sum >> 16);
    }
    return static_cast<uint16_t>(~sum);
}

inline uint16_t tcp_checksum(const uint8_t source_ip[4],
                             const uint8_t destination_ip[4],
                             const uint8_t* tcp,
                             size_t tcp_length) {
    uint32_t sum = 0;
    sum += checksum_partial(source_ip, 4);
    sum += checksum_partial(destination_ip, 4);
    sum += static_cast<uint32_t>(kIpv4ProtocolTcp);
    sum += static_cast<uint32_t>(tcp_length);
    sum += checksum_partial(tcp, tcp_length);
    return finish_checksum(sum);
}

inline bool parse_tcp_frame(const uint8_t* frame,
                            size_t frame_length,
                            TcpSegmentView& out) {
    if (frame == nullptr || frame_length < kEthernetHeaderSize +
                                            kIpv4HeaderMinSize +
                                            kTcpHeaderMinSize) {
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
        frame_length < kEthernetHeaderSize + ihl + kTcpHeaderMinSize ||
        ipv4[9] != kIpv4ProtocolTcp) {
        return false;
    }

    uint16_t total_length = load_be16(ipv4 + 2);
    if (total_length < ihl + kTcpHeaderMinSize ||
        frame_length < kEthernetHeaderSize + total_length) {
        return false;
    }

    uint16_t fragment = load_be16(ipv4 + 6);
    if ((fragment & 0x3FFFu) != 0) {
        return false;
    }

    const uint8_t* tcp = ipv4 + ihl;
    size_t tcp_length = total_length - ihl;
    uint8_t data_offset = static_cast<uint8_t>((tcp[12] >> 4) * 4u);
    if (data_offset < kTcpHeaderMinSize || data_offset > tcp_length) {
        return false;
    }

    out.options = tcp + kTcpHeaderMinSize;
    out.options_length = data_offset - kTcpHeaderMinSize;
    out.payload = tcp + data_offset;
    out.payload_length = tcp_length - data_offset;
    for (size_t i = 0; i < 4; ++i) {
        out.source_ip[i] = ipv4[12 + i];
        out.destination_ip[i] = ipv4[16 + i];
    }
    out.source_port = load_be16(tcp + 0);
    out.destination_port = load_be16(tcp + 2);
    out.sequence_number = load_be32(tcp + 4);
    out.acknowledgment_number = load_be32(tcp + 8);
    out.flags = load_be16(tcp + 12) & 0x01FFu;
    out.window_size = load_be16(tcp + 14);
    return true;
}

inline bool build_tcp_ipv4_frame(uint8_t* out_frame,
                                 size_t out_capacity,
                                 size_t& out_length,
                                 const uint8_t source_mac[6],
                                 const uint8_t destination_mac[6],
                                 const uint8_t source_ip[4],
                                 const uint8_t destination_ip[4],
                                 uint16_t source_port,
                                 uint16_t destination_port,
                                 uint32_t sequence_number,
                                 uint32_t acknowledgment_number,
                                 uint16_t flags,
                                 uint16_t window_size,
                                 const void* options,
                                 size_t options_length,
                                 const void* payload,
                                 size_t payload_length) {
    out_length = 0;
    if (out_frame == nullptr ||
        options_length > kTcpMaxOptionBytes ||
        (options_length % 4) != 0 ||
        payload_length > kTcpMaxPayload ||
        (options_length != 0 && options == nullptr) ||
        (payload_length != 0 && payload == nullptr)) {
        return false;
    }

    size_t tcp_header_length = kTcpHeaderMinSize + options_length;
    size_t ipv4_payload_length = tcp_header_length + payload_length;
    size_t total_length = kEthernetHeaderSize + kIpv4HeaderMinSize + ipv4_payload_length;
    if (total_length > out_capacity || ipv4_payload_length > 1480) {
        return false;
    }

    memcpy(out_frame, destination_mac, 6);
    memcpy(out_frame + 6, source_mac, 6);
    store_be16(out_frame + 12, kEtherTypeIpv4);

    uint8_t* ipv4 = out_frame + kEthernetHeaderSize;
    ipv4[0] = 0x45;
    ipv4[1] = 0x00;
    store_be16(ipv4 + 2, static_cast<uint16_t>(kIpv4HeaderMinSize + ipv4_payload_length));
    store_be16(ipv4 + 4, 0);
    store_be16(ipv4 + 6, 0);
    ipv4[8] = 64;
    ipv4[9] = kIpv4ProtocolTcp;
    store_be16(ipv4 + 10, 0);
    memcpy(ipv4 + 12, source_ip, 4);
    memcpy(ipv4 + 16, destination_ip, 4);
    store_be16(ipv4 + 10, internet_checksum(ipv4, kIpv4HeaderMinSize));

    uint8_t* tcp = ipv4 + kIpv4HeaderMinSize;
    store_be16(tcp + 0, source_port);
    store_be16(tcp + 2, destination_port);
    store_be32(tcp + 4, sequence_number);
    store_be32(tcp + 8, acknowledgment_number);
    store_be16(tcp + 12,
               static_cast<uint16_t>(((tcp_header_length / 4u) << 12) |
                                     (flags & 0x01FFu)));
    store_be16(tcp + 14, window_size);
    store_be16(tcp + 16, 0);
    store_be16(tcp + 18, 0);

    const uint8_t* option_bytes = static_cast<const uint8_t*>(options);
    if (options_length != 0) {
        memcpy(tcp + kTcpHeaderMinSize, option_bytes, options_length);
    }

    const uint8_t* payload_bytes = static_cast<const uint8_t*>(payload);
    if (payload_length != 0) {
        memcpy(tcp + tcp_header_length, payload_bytes, payload_length);
    }

    store_be16(tcp + 16, tcp_checksum(source_ip, destination_ip, tcp, ipv4_payload_length));
    out_length = total_length;
    return true;
}

}  // namespace usernet
