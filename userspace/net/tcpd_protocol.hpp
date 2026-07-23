#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

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
    uint32_t inbound_syns;
    uint32_t syn_ack_retransmits;
    uint32_t established;
    uint32_t wait_ack_mismatch;
    uint32_t remote_resets;
    uint32_t remote_fins;
    uint32_t last_flags;
    uint32_t last_seq;
    uint32_t last_ack;
    uint32_t expected_seq;
    uint32_t expected_ack;
    // Appended diagnostics preserve the stable version-1 registry prefix.
    uint32_t outbound_syns;
    uint32_t outbound_syn_retransmits;
    uint32_t outbound_connect_timeouts;
    uint32_t syn_sent;
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
    uint32_t endpoint_id;
};

struct ConnectResponse {
    int32_t status;
    uint32_t connection_id;
    uint16_t local_port;
    uint16_t reserved;
    uint32_t endpoint_id;
};

struct AcceptEvent {
    uint32_t connection_id;
    uint16_t local_port;
    uint16_t remote_port;
    uint8_t remote_ip[4];
    uint32_t endpoint_id;
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
    memset(&message, 0, sizeof(Message));
    message.magic = kMessageMagic;
    message.version = kMessageVersion;
    message.type = type;
}

inline bool write_message(uint32_t handle, const Message& message) {
    static Message* bounce = nullptr;
    if (bounce == nullptr) {
        bounce = static_cast<Message*>(
            map_anonymous(sizeof(Message), MAP_WRITE));
    }
    if (bounce == nullptr) {
        return false;
    }
    uint8_t* bounce_bytes = reinterpret_cast<uint8_t*>(bounce);
    memcpy(bounce_bytes, &message, sizeof(Message));
    size_t written = 0;
    descriptor_defs::DescriptorWait wait{};
    wait.handle = handle;
    wait.events = descriptor_defs::kWaitWrite;
    wait.revents = 0;
    wait.reserved = 0;
    while (written < sizeof(Message)) {
        long result = descriptor_write(handle,
                                       bounce_bytes + written,
                                       sizeof(Message) - written);
        if (result == kDescriptorWouldBlock) {
            wait.revents = 0;
            if (descriptor_wait(&wait, 1) < 0) {
                yield();
            }
            continue;
        }
        if (result <= 0) {
            return false;
        }
        written += static_cast<size_t>(result);
    }
    return true;
}

inline bool read_message(uint32_t handle, Message& message) {
    static Message* bounce = nullptr;
    if (bounce == nullptr) {
        bounce = static_cast<Message*>(
            map_anonymous(sizeof(Message), MAP_WRITE));
    }
    if (bounce == nullptr) {
        return false;
    }
    uint8_t* bounce_bytes = reinterpret_cast<uint8_t*>(bounce);
    size_t total = 0;
    descriptor_defs::DescriptorWait wait{};
    wait.handle = handle;
    wait.events = descriptor_defs::kWaitRead;
    wait.revents = 0;
    wait.reserved = 0;
    while (total < sizeof(Message)) {
        long result = descriptor_read(handle,
                                      bounce_bytes + total,
                                      sizeof(Message) - total);
        if (result == kDescriptorWouldBlock) {
            if (total == 0) {
                return false;
            }
            wait.revents = 0;
            if (descriptor_wait(&wait, 1) < 0) {
                yield();
            }
            continue;
        }
        if (result <= 0) {
            return false;
        }
        total += static_cast<size_t>(result);
    }
    memcpy(&message, bounce_bytes, sizeof(Message));
    bool ok = message.magic == kMessageMagic &&
              message.version == kMessageVersion;
    return ok;
}

}  // namespace tcpd_protocol
