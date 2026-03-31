#pragma once

#include <stddef.h>
#include <stdint.h>

#include "../crt/syscall.hpp"
#include "descriptors.hpp"

namespace networkd_protocol {

constexpr uint32_t kRegistryMagic = 0x4E455457u;  // "NETW"
constexpr uint32_t kRegistryVersion = 2;
constexpr const char kRegistryName[] = "network.registry";

constexpr uint32_t kMessageMagic = 0x4E455450u;  // "NETP"
constexpr uint16_t kMessageVersion = 1;
// Allow a full non-fragmented IPv4/UDP payload over Ethernet so DHCP
// replies and future protocols are not truncated by the userspace IPC layer.
constexpr size_t kMaxUdpPayload = 1472;

enum MessageType : uint16_t {
    kBindUdpRequest = 1,
    kSendUdpRequest = 2,
    kSendIcmpEchoRequest = 3,
    kBindUdpResponse = 0x8001,
    kSendUdpResponse = 0x8002,
    kUdpPacketEvent = 0x8003,
    kIcmpEchoReplyEvent = 0x8004,
};

enum SendFlags : uint16_t {
    kSendFlagBroadcast = 1u << 0,
};

enum Status : int32_t {
    kStatusOk = 0,
    kStatusInvalid = -1,
    kStatusInUse = -2,
    kStatusNotFound = -3,
    kStatusTooLarge = -4,
    kStatusIo = -5,
};

struct Registry {
    uint32_t magic;
    uint32_t version;
    uint32_t server_pipe_id;
    uint32_t reserved;
    uint32_t networkd_state;
    uint32_t dhcp_state;
    uint32_t dhcp_attempts;
    uint32_t dhcp_last_offer;
    uint32_t dhcp_last_ack;
    uint32_t dhcp_last_error;
    uint32_t net_rx_frames;
    uint32_t net_rx_udp;
    uint32_t net_rx_delivered;
    uint32_t net_tx_udp;
};

enum RegistryState : uint32_t {
    kStateIdle = 0,
    kStateStarting = 1,
    kStateReady = 2,
    kStateWaitingLink = 3,
    kStateBound = 4,
    kStateReplyPipeReady = 5,
    kStateServerPipeOpen = 6,
    kStateBindSent = 7,
    kStateBindSeen = 8,
    kStateBindReplySent = 9,
    kStateDiscoverSent = 10,
    kStateOfferReceived = 11,
    kStateRequestSent = 12,
    kStateAckReceived = 13,
    kStateLeaseApplied = 14,
    kStateWaitingOffer = 15,
    kStateError = 0x80000000u,
};

enum RegistryError : uint32_t {
    kErrorNone = 0,
    kErrorOpenDevice = 1,
    kErrorOpenRegistry = 2,
    kErrorCreateReplyPipe = 3,
    kErrorReplyPipeInfo = 4,
    kErrorOpenServerPipe = 5,
    kErrorBindSend = 6,
    kErrorBindResponse = 7,
    kErrorSendDiscover = 8,
    kErrorNoOffer = 9,
    kErrorSendRequest = 10,
    kErrorNoAck = 11,
    kErrorApplyConfig = 12,
};

struct BindUdpRequest {
    uint32_t reply_pipe_id;
    uint16_t port;
    uint16_t reserved;
};

struct BindUdpResponse {
    int32_t status;
    uint16_t port;
    uint16_t reserved;
};

struct SendUdpRequest {
    uint16_t source_port;
    uint16_t destination_port;
    uint16_t flags;
    uint16_t payload_length;
    uint8_t source_ip[4];
    uint8_t destination_ip[4];
    uint8_t payload[kMaxUdpPayload];
};

struct SendUdpResponse {
    int32_t status;
    uint32_t reserved;
};

struct SendIcmpEchoRequest {
    uint32_t reply_pipe_id;
    uint16_t identifier;
    uint16_t sequence;
    uint16_t flags;
    uint16_t payload_length;
    uint8_t destination_ip[4];
    uint8_t payload[kMaxUdpPayload];
};

struct UdpPacketEvent {
    uint16_t source_port;
    uint16_t destination_port;
    uint16_t payload_length;
    uint16_t reserved;
    uint8_t source_ip[4];
    uint8_t destination_ip[4];
    uint8_t payload[kMaxUdpPayload];
};

struct IcmpEchoReplyEvent {
    uint16_t identifier;
    uint16_t sequence;
    uint16_t payload_length;
    uint8_t ttl;
    uint8_t source_ip[4];
    uint8_t destination_ip[4];
    uint8_t payload[kMaxUdpPayload];
};

struct Message {
    uint32_t magic;
    uint16_t version;
    uint16_t type;
    union {
        BindUdpRequest bind_request;
        BindUdpResponse bind_response;
        SendUdpRequest send_request;
        SendUdpResponse send_response;
        UdpPacketEvent udp_event;
        SendIcmpEchoRequest icmp_request;
        IcmpEchoReplyEvent icmp_event;
    };
};

inline void init_message(Message& message, uint16_t type) {
    for (size_t i = 0; i < sizeof(Message); ++i) {
        reinterpret_cast<uint8_t*>(&message)[i] = 0;
    }
    message.magic = kMessageMagic;
    message.version = kMessageVersion;
    message.type = type;
}

inline bool write_message(uint32_t handle, const Message& message) {
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&message);
    size_t written = 0;
    while (written < sizeof(Message)) {
        long result = descriptor_write(handle,
                                       bytes + written,
                                       sizeof(Message) - written);
        if (result <= 0) {
            return false;
        }
        written += static_cast<size_t>(result);
    }
    return true;
}

inline bool read_message(uint32_t handle, Message& message) {
    uint8_t* bytes = reinterpret_cast<uint8_t*>(&message);
    size_t total = 0;
    while (total < sizeof(Message)) {
        long result = descriptor_read(handle, bytes + total, sizeof(Message) - total);
        if (result == kDescriptorWouldBlock) {
            if (total == 0) {
                return false;
            }
            yield();
            continue;
        }
        if (result <= 0) {
            return false;
        }
        total += static_cast<size_t>(result);
    }
    return message.magic == kMessageMagic &&
           message.version == kMessageVersion;
}

}  // namespace networkd_protocol
