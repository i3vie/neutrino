#pragma once

#include <stddef.h>
#include <stdint.h>

#include "../crt/syscall.hpp"
#include "descriptors.hpp"

namespace usernet {

constexpr uint16_t kEtherTypeIpv4 = 0x0800;
constexpr uint8_t kIpv4ProtocolUdp = 17;
constexpr size_t kEthernetHeaderSize = 14;
constexpr size_t kIpv4HeaderMinSize = 20;
constexpr size_t kUdpHeaderSize = 8;
constexpr size_t kMaxFrameSize = 1600;

struct Device {
    uint32_t handle = kInvalidDescriptor;
    descriptor_defs::NetDeviceInfo info{};
};

struct UdpPacketView {
    const uint8_t* payload;
    size_t payload_length;
    uint8_t source_ip[4];
    uint8_t destination_ip[4];
    uint16_t source_port;
    uint16_t destination_port;
};

inline uint16_t load_be16(const uint8_t* ptr) {
    return static_cast<uint16_t>((static_cast<uint16_t>(ptr[0]) << 8) |
                                 static_cast<uint16_t>(ptr[1]));
}

inline void store_be16(uint8_t* ptr, uint16_t value) {
    ptr[0] = static_cast<uint8_t>((value >> 8) & 0xFFu);
    ptr[1] = static_cast<uint8_t>(value & 0xFFu);
}

inline uint32_t load_be32(const uint8_t* ptr) {
    return (static_cast<uint32_t>(ptr[0]) << 24) |
           (static_cast<uint32_t>(ptr[1]) << 16) |
           (static_cast<uint32_t>(ptr[2]) << 8) |
           static_cast<uint32_t>(ptr[3]);
}

inline void store_be32(uint8_t* ptr, uint32_t value) {
    ptr[0] = static_cast<uint8_t>((value >> 24) & 0xFFu);
    ptr[1] = static_cast<uint8_t>((value >> 16) & 0xFFu);
    ptr[2] = static_cast<uint8_t>((value >> 8) & 0xFFu);
    ptr[3] = static_cast<uint8_t>(value & 0xFFu);
}

inline uint16_t internet_checksum(const void* data, size_t length) {
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    uint32_t sum = 0;
    while (length > 1) {
        sum += static_cast<uint32_t>((static_cast<uint16_t>(bytes[0]) << 8) |
                                     static_cast<uint16_t>(bytes[1]));
        bytes += 2;
        length -= 2;
    }
    if (length != 0) {
        sum += static_cast<uint32_t>(static_cast<uint16_t>(bytes[0]) << 8);
    }
    while ((sum >> 16) != 0) {
        sum = (sum & 0xFFFFu) + (sum >> 16);
    }
    return static_cast<uint16_t>(~sum);
}

inline bool open_device(Device& device,
                        uint32_t index = 0,
                        uint64_t requested_flags = 0) {
    long handle = net_device_open(index, requested_flags);
    if (handle < 0) {
        return false;
    }
    device.handle = static_cast<uint32_t>(handle);
    if (net_device_get_info(device.handle, &device.info) != 0) {
        descriptor_close(device.handle);
        device.handle = kInvalidDescriptor;
        return false;
    }
    return true;
}

inline void close_device(Device& device) {
    if (device.handle != kInvalidDescriptor) {
        descriptor_close(device.handle);
        device.handle = kInvalidDescriptor;
    }
}

inline bool parse_udp_frame(const uint8_t* frame,
                            size_t frame_length,
                            UdpPacketView& out) {
    if (frame == nullptr || frame_length < kEthernetHeaderSize +
                                            kIpv4HeaderMinSize +
                                            kUdpHeaderSize) {
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
        frame_length < kEthernetHeaderSize + ihl + kUdpHeaderSize ||
        ipv4[9] != kIpv4ProtocolUdp) {
        return false;
    }

    uint16_t total_length = load_be16(ipv4 + 2);
    if (total_length < ihl + kUdpHeaderSize ||
        frame_length < kEthernetHeaderSize + total_length) {
        return false;
    }

    uint16_t fragment = load_be16(ipv4 + 6);
    if ((fragment & 0x3FFFu) != 0) {
        return false;
    }

    const uint8_t* udp = ipv4 + ihl;
    uint16_t udp_length = load_be16(udp + 4);
    if (udp_length < kUdpHeaderSize || udp_length > total_length - ihl) {
        return false;
    }

    out.payload = udp + kUdpHeaderSize;
    out.payload_length = udp_length - kUdpHeaderSize;
    for (size_t i = 0; i < 4; ++i) {
        out.source_ip[i] = ipv4[12 + i];
        out.destination_ip[i] = ipv4[16 + i];
    }
    out.source_port = load_be16(udp + 0);
    out.destination_port = load_be16(udp + 2);
    return true;
}

inline bool build_udp_ipv4_frame(uint8_t* out_frame,
                                 size_t out_capacity,
                                 size_t& out_length,
                                 const uint8_t source_mac[6],
                                 const uint8_t destination_mac[6],
                                 const uint8_t source_ip[4],
                                 const uint8_t destination_ip[4],
                                 uint16_t source_port,
                                 uint16_t destination_port,
                                 const void* payload,
                                 size_t payload_length) {
    out_length = 0;
    if (out_frame == nullptr || payload_length > 1472 || payload == nullptr) {
        return false;
    }

    size_t total_length =
        kEthernetHeaderSize + kIpv4HeaderMinSize + kUdpHeaderSize + payload_length;
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
    ipv4[1] = 0x00;
    store_be16(ipv4 + 2,
               static_cast<uint16_t>(kIpv4HeaderMinSize + kUdpHeaderSize +
                                     payload_length));
    store_be16(ipv4 + 4, 0);
    store_be16(ipv4 + 6, 0);
    ipv4[8] = 64;
    ipv4[9] = kIpv4ProtocolUdp;
    store_be16(ipv4 + 10, 0);
    for (size_t i = 0; i < 4; ++i) {
        ipv4[12 + i] = source_ip[i];
        ipv4[16 + i] = destination_ip[i];
    }
    store_be16(ipv4 + 10, internet_checksum(ipv4, kIpv4HeaderMinSize));

    uint8_t* udp = ipv4 + kIpv4HeaderMinSize;
    store_be16(udp + 0, source_port);
    store_be16(udp + 2, destination_port);
    store_be16(udp + 4, static_cast<uint16_t>(kUdpHeaderSize + payload_length));
    store_be16(udp + 6, 0);

    const uint8_t* payload_bytes = static_cast<const uint8_t*>(payload);
    for (size_t i = 0; i < payload_length; ++i) {
        udp[kUdpHeaderSize + i] = payload_bytes[i];
    }

    // IPv4 permits a zero UDP checksum. Keep transmit simple during bring-up,
    // especially for DHCP broadcast traffic, until the stack is hardware-validated.
    store_be16(udp + 6, 0);

    out_length = total_length;
    return true;
}

}  // namespace usernet
