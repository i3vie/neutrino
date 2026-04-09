#pragma once

#include <stddef.h>
#include <stdint.h>

#include "../crt/syscall.hpp"
#include "descriptors.hpp"
#include "network_protocol.hpp"

namespace tcpd_protocol {

constexpr uint32_t kRegistryMagic = 0x54435044u;  // "TCPD"
constexpr uint32_t kRegistryVersion = 1;
constexpr const char kRegistryName[] = "tcp.registry";

constexpr uint32_t kMessageMagic = 0x54435050u;  // "TCPP"
constexpr uint16_t kMessageVersion = 1;
constexpr size_t kMaxPayload = networkd_protocol::kMaxTcpPayload;

enum MessageType : uint16_t {
    kListenRequest = 1,
    kSendRequest = 2,
    kCloseRequest = 3,
    kConnectRequest = 4,
    kListenResponse = 0x8001,
    kAcceptEvent = 0x8002,
    kDataEvent = 0x8003,
    kClosedEvent = 0x8004,
    kConnectResponse = 0x8005,
};

enum Status : int32_t {
    kStatusOk = 0,
    kStatusInvalid = -1,
    kStatusInUse = -2,
    kStatusNotFound = -3,
    kStatusIo = -4,
};

enum RegistryState : uint32_t {
    kStateIdle = 0,
    kStateStarting = 1,
    kStateReady = 2,
    kStateError = 0x80000000u,
};

enum CloseReason : uint32_t {
    kCloseRemoteFin = 1,
    kCloseRemoteRst = 2,
    kCloseLocalClose = 3,
    kCloseProtocolError = 4,
};

struct Registry {
    uint32_t magic;
    uint32_t version;
    uint32_t server_pipe_id;
    uint32_t state;
    uint32_t listeners;
    uint32_t connections;
    uint32_t rx_segments;
    uint32_t tx_segments;
};

struct ListenRequest {
    uint32_t reply_pipe_id;
    uint16_t port;
    uint16_t reserved;
};

struct ListenResponse {
    int32_t status;
    uint16_t port;
    uint16_t reserved;
};

struct SendRequest {
    uint32_t connection_id;
    uint16_t payload_length;
    uint16_t reserved;
    uint8_t payload[kMaxPayload];
};

struct CloseRequest {
    uint32_t connection_id;
    uint32_t reserved;
};

struct ConnectRequest {
    uint32_t reply_pipe_id;
    uint16_t remote_port;
    uint16_t reserved;
    uint8_t remote_ip[4];
    uint8_t reserved2[4];
};

struct ConnectResponse {
    int32_t status;
    uint32_t connection_id;
    uint16_t local_port;
    uint16_t reserved;
};

struct AcceptEvent {
    uint32_t connection_id;
    uint16_t local_port;
    uint16_t remote_port;
    uint8_t remote_ip[4];
    uint8_t reserved[4];
};

struct DataEvent {
    uint32_t connection_id;
    uint16_t payload_length;
    uint16_t reserved;
    uint8_t payload[kMaxPayload];
};

struct ClosedEvent {
    uint32_t connection_id;
    uint32_t reason;
};

struct Message {
    uint32_t magic;
    uint16_t version;
    uint16_t type;
    union {
        ListenRequest listen_request;
        ListenResponse listen_response;
        SendRequest send_request;
        CloseRequest close_request;
        ConnectRequest connect_request;
        ConnectResponse connect_response;
        AcceptEvent accept_event;
        DataEvent data_event;
        ClosedEvent closed_event;
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
        long result = descriptor_write(handle, bytes + written, sizeof(Message) - written);
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

}  // namespace tcpd_protocol
