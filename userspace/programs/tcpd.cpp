#include <stddef.h>
#include <stdint.h>

#include "../crt/syscall.hpp"
#include "../net/network_protocol.hpp"
#include "../net/tcp.hpp"
#include "../net/tcpd_protocol.hpp"
#include "../net/udp.hpp"

namespace {

constexpr size_t kMaxListeners = 8;
constexpr size_t kMaxConnections = 32;
constexpr size_t kMaxClientPorts = 8;
constexpr size_t kMaxPendingConnects = 8;
constexpr uint16_t kDefaultWindowSize = 4096;
constexpr uint16_t kEphemeralPortStart = 49152;
constexpr uint16_t kEphemeralPortEnd = 65535;

enum ConnectionState : uint8_t {
    kConnStateClosed = 0,
    kConnStateSynReceived = 1,
    kConnStateSynSent = 2,
    kConnStateEstablished = 3,
};

struct Listener {
    bool in_use;
    bool bound;
    uint16_t port;
    uint32_t app_pipe_id;
    uint32_t app_pipe_handle;
};

struct Connection {
    bool in_use;
    bool own_app_pipe_handle;
    uint32_t id;
    uint8_t state;
    uint8_t remote_ip[4];
    uint16_t local_port;
    uint16_t remote_port;
    uint32_t remote_next_seq;
    uint32_t local_next_seq;
    uint32_t app_pipe_handle;
    Listener* listener;
};

struct ClientPort {
    bool in_use;
    bool bound;
    bool binding_in_progress;
    uint16_t port;
};

struct PendingConnect {
    bool in_use;
    uint32_t app_pipe_handle;
    uint16_t local_port;
    uint16_t remote_port;
    uint8_t remote_ip[4];
};

struct ServerContext {
    usernet::Device device;
    uint32_t tcpd_server_pipe;
    uint32_t networkd_server_pipe;
    uint32_t network_reply_pipe;
    uint32_t network_reply_pipe_id;
    tcpd_protocol::Registry* registry;
    Listener listeners[kMaxListeners];
    Connection connections[kMaxConnections];
    ClientPort client_ports[kMaxClientPorts];
    PendingConnect pending_connects[kMaxPendingConnects];
    uint32_t next_connection_id;
};

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

bool load_ipv4_config(ServerContext& ctx, descriptor_defs::NetIpv4Config& out) {
    return net_device_get_ipv4_config(ctx.device.handle, &out) == 0 &&
           (out.flags & descriptor_defs::kNetIpv4FlagEnabled) != 0;
}

Listener* find_listener(ServerContext& ctx, uint16_t port) {
    for (size_t i = 0; i < kMaxListeners; ++i) {
        if (ctx.listeners[i].in_use && ctx.listeners[i].port == port) {
            return &ctx.listeners[i];
        }
    }
    return nullptr;
}

Listener* allocate_listener(ServerContext& ctx) {
    for (size_t i = 0; i < kMaxListeners; ++i) {
        if (!ctx.listeners[i].in_use) {
            return &ctx.listeners[i];
        }
    }
    return nullptr;
}

Connection* find_connection(ServerContext& ctx, uint32_t id) {
    for (size_t i = 0; i < kMaxConnections; ++i) {
        if (ctx.connections[i].in_use && ctx.connections[i].id == id) {
            return &ctx.connections[i];
        }
    }
    return nullptr;
}

Connection* find_connection_by_tuple(ServerContext& ctx,
                                     uint16_t local_port,
                                     const uint8_t remote_ip[4],
                                     uint16_t remote_port) {
    for (size_t i = 0; i < kMaxConnections; ++i) {
        Connection& conn = ctx.connections[i];
        if (!conn.in_use ||
            conn.local_port != local_port ||
            conn.remote_port != remote_port) {
            continue;
        }
        bool match = true;
        for (size_t j = 0; j < 4; ++j) {
            if (conn.remote_ip[j] != remote_ip[j]) {
                match = false;
                break;
            }
        }
        if (match) {
            return &conn;
        }
    }
    return nullptr;
}

Connection* allocate_connection(ServerContext& ctx) {
    for (size_t i = 0; i < kMaxConnections; ++i) {
        if (!ctx.connections[i].in_use) {
            return &ctx.connections[i];
        }
    }
    return nullptr;
}

ClientPort* find_client_port(ServerContext& ctx, uint16_t port) {
    for (size_t i = 0; i < kMaxClientPorts; ++i) {
        if (ctx.client_ports[i].in_use && ctx.client_ports[i].port == port) {
            return &ctx.client_ports[i];
        }
    }
    return nullptr;
}

ClientPort* allocate_client_port(ServerContext& ctx) {
    for (size_t i = 0; i < kMaxClientPorts; ++i) {
        if (!ctx.client_ports[i].in_use) {
            return &ctx.client_ports[i];
        }
    }
    return nullptr;
}

PendingConnect* find_pending_connect(ServerContext& ctx, uint16_t local_port) {
    for (size_t i = 0; i < kMaxPendingConnects; ++i) {
        if (ctx.pending_connects[i].in_use &&
            ctx.pending_connects[i].local_port == local_port) {
            return &ctx.pending_connects[i];
        }
    }
    return nullptr;
}

PendingConnect* allocate_pending_connect(ServerContext& ctx) {
    for (size_t i = 0; i < kMaxPendingConnects; ++i) {
        if (!ctx.pending_connects[i].in_use) {
            return &ctx.pending_connects[i];
        }
    }
    return nullptr;
}

void recount_registry(ServerContext& ctx) {
    if (ctx.registry == nullptr) {
        return;
    }
    uint32_t listeners = 0;
    uint32_t connections = 0;
    for (size_t i = 0; i < kMaxListeners; ++i) {
        if (ctx.listeners[i].in_use && ctx.listeners[i].bound) {
            ++listeners;
        }
    }
    for (size_t i = 0; i < kMaxConnections; ++i) {
        if (ctx.connections[i].in_use) {
            ++connections;
        }
    }
    ctx.registry->listeners = listeners;
    ctx.registry->connections = connections;
}

bool populate_registry(ServerContext& ctx, uint32_t server_pipe_id) {
    long handle =
        shared_memory_open(tcpd_protocol::kRegistryName,
                           sizeof(tcpd_protocol::Registry));
    if (handle < 0) {
        return false;
    }
    descriptor_defs::SharedMemoryInfo info{};
    if (shared_memory_get_info(static_cast<uint32_t>(handle), &info) != 0 ||
        info.base == 0 ||
        info.length < sizeof(tcpd_protocol::Registry)) {
        descriptor_close(static_cast<uint32_t>(handle));
        return false;
    }

    auto* registry = reinterpret_cast<tcpd_protocol::Registry*>(info.base);
    registry->magic = tcpd_protocol::kRegistryMagic;
    registry->version = tcpd_protocol::kRegistryVersion;
    registry->server_pipe_id = server_pipe_id;
    registry->state = tcpd_protocol::kStateStarting;
    registry->listeners = 0;
    registry->connections = 0;
    registry->rx_segments = 0;
    registry->tx_segments = 0;
    ctx.registry = registry;
    return true;
}

bool send_network_segment(ServerContext& ctx,
                          const uint8_t source_ip[4],
                          const uint8_t destination_ip[4],
                          uint16_t source_port,
                          uint16_t destination_port,
                          uint32_t sequence_number,
                          uint32_t acknowledgment_number,
                          uint16_t flags,
                          const uint8_t* payload,
                          size_t payload_length) {
    if (payload_length > networkd_protocol::kMaxTcpPayload) {
        return false;
    }
    networkd_protocol::Message message{};
    networkd_protocol::init_message(message, networkd_protocol::kSendTcpRequest);
    message.send_tcp_request.source_port = source_port;
    message.send_tcp_request.destination_port = destination_port;
    message.send_tcp_request.flags = flags;
    message.send_tcp_request.options_length = 0;
    message.send_tcp_request.payload_length = static_cast<uint16_t>(payload_length);
    message.send_tcp_request.window_size = kDefaultWindowSize;
    message.send_tcp_request.sequence_number = sequence_number;
    message.send_tcp_request.acknowledgment_number = acknowledgment_number;
    for (size_t i = 0; i < 4; ++i) {
        message.send_tcp_request.source_ip[i] = source_ip[i];
        message.send_tcp_request.destination_ip[i] = destination_ip[i];
    }
    for (size_t i = 0; i < payload_length; ++i) {
        message.send_tcp_request.payload[i] = payload[i];
    }
    if (!networkd_protocol::write_message(ctx.networkd_server_pipe, message)) {
        return false;
    }
    if (ctx.registry != nullptr) {
        ++ctx.registry->tx_segments;
    }
    return true;
}

void send_listen_response(Listener& listener, int32_t status) {
    tcpd_protocol::Message message{};
    tcpd_protocol::init_message(message, tcpd_protocol::kListenResponse);
    message.listen_response.status = status;
    message.listen_response.port = listener.port;
    (void)tcpd_protocol::write_message(listener.app_pipe_handle, message);
}

void send_connect_response(uint32_t handle,
                           int32_t status,
                           uint32_t connection_id,
                           uint16_t local_port) {
    tcpd_protocol::Message message{};
    tcpd_protocol::init_message(message, tcpd_protocol::kConnectResponse);
    message.connect_response.status = status;
    message.connect_response.connection_id = connection_id;
    message.connect_response.local_port = local_port;
    (void)tcpd_protocol::write_message(handle, message);
}

void send_accept_event(Connection& conn) {
    tcpd_protocol::Message message{};
    tcpd_protocol::init_message(message, tcpd_protocol::kAcceptEvent);
    message.accept_event.connection_id = conn.id;
    message.accept_event.local_port = conn.local_port;
    message.accept_event.remote_port = conn.remote_port;
    for (size_t i = 0; i < 4; ++i) {
        message.accept_event.remote_ip[i] = conn.remote_ip[i];
    }
    (void)tcpd_protocol::write_message(conn.app_pipe_handle, message);
}

void send_data_event(Connection& conn, const uint8_t* payload, size_t payload_length) {
    if (payload_length > tcpd_protocol::kMaxPayload) {
        return;
    }
    tcpd_protocol::Message message{};
    tcpd_protocol::init_message(message, tcpd_protocol::kDataEvent);
    message.data_event.connection_id = conn.id;
    message.data_event.payload_length = static_cast<uint16_t>(payload_length);
    for (size_t i = 0; i < payload_length; ++i) {
        message.data_event.payload[i] = payload[i];
    }
    (void)tcpd_protocol::write_message(conn.app_pipe_handle, message);
}

void send_closed_event(Connection& conn, uint32_t reason) {
    tcpd_protocol::Message message{};
    tcpd_protocol::init_message(message, tcpd_protocol::kClosedEvent);
    message.closed_event.connection_id = conn.id;
    message.closed_event.reason = reason;
    (void)tcpd_protocol::write_message(conn.app_pipe_handle, message);
}

void reset_connection(Connection& conn) {
    conn.in_use = false;
    conn.own_app_pipe_handle = false;
    conn.id = 0;
    conn.state = kConnStateClosed;
    conn.local_port = 0;
    conn.remote_port = 0;
    conn.remote_next_seq = 0;
    conn.local_next_seq = 0;
    conn.app_pipe_handle = 0;
    conn.listener = nullptr;
}

void close_connection(ServerContext& ctx, Connection& conn, uint32_t reason) {
    send_closed_event(conn, reason);
    if (conn.own_app_pipe_handle && conn.app_pipe_handle != 0) {
        descriptor_close(conn.app_pipe_handle);
    }
    reset_connection(conn);
    recount_registry(ctx);
}

bool is_outbound_port_busy(ServerContext& ctx, uint16_t port) {
    if (find_listener(ctx, port) != nullptr ||
        find_pending_connect(ctx, port) != nullptr) {
        return true;
    }
    for (size_t i = 0; i < kMaxConnections; ++i) {
        if (ctx.connections[i].in_use &&
            ctx.connections[i].local_port == port) {
            return true;
        }
    }
    return false;
}

uint16_t choose_client_port(ServerContext& ctx) {
    for (size_t i = 0; i < kMaxClientPorts; ++i) {
        ClientPort& port = ctx.client_ports[i];
        if (port.in_use &&
            port.bound &&
            !port.binding_in_progress &&
            !is_outbound_port_busy(ctx, port.port)) {
            return port.port;
        }
    }

    for (uint32_t port = kEphemeralPortStart; port <= kEphemeralPortEnd; ++port) {
        if (find_client_port(ctx, static_cast<uint16_t>(port)) != nullptr) {
            continue;
        }
        if (!is_outbound_port_busy(ctx, static_cast<uint16_t>(port))) {
            return static_cast<uint16_t>(port);
        }
    }
    return 0;
}

bool begin_outbound_connection(ServerContext& ctx,
                               uint32_t app_pipe_handle,
                               uint16_t local_port,
                               const uint8_t remote_ip[4],
                               uint16_t remote_port) {
    Connection* conn = allocate_connection(ctx);
    if (conn == nullptr) {
        return false;
    }

    descriptor_defs::NetIpv4Config cfg{};
    if (!load_ipv4_config(ctx, cfg)) {
        return false;
    }

    conn->in_use = true;
    conn->own_app_pipe_handle = true;
    conn->id = ++ctx.next_connection_id;
    conn->state = kConnStateSynSent;
    conn->local_port = local_port;
    conn->remote_port = remote_port;
    conn->remote_next_seq = 0;
    conn->app_pipe_handle = app_pipe_handle;
    conn->listener = nullptr;
    for (size_t i = 0; i < 4; ++i) {
        conn->remote_ip[i] = remote_ip[i];
    }

    uint32_t initial_seq =
        0x4E540000u + (static_cast<uint32_t>(local_port) << 4) + conn->id;
    conn->local_next_seq = initial_seq + 1;
    if (!send_network_segment(ctx,
                              cfg.address,
                              conn->remote_ip,
                              conn->local_port,
                              conn->remote_port,
                              initial_seq,
                              0,
                              usernet::kTcpFlagSyn,
                              nullptr,
                              0)) {
        reset_connection(*conn);
        return false;
    }

    recount_registry(ctx);
    return true;
}

void handle_listen_request(ServerContext& ctx,
                           const tcpd_protocol::ListenRequest& request) {
    if (request.reply_pipe_id == 0 || request.port == 0) {
        return;
    }
    Listener* existing = find_listener(ctx, request.port);
    if (existing != nullptr) {
        uint64_t flags = static_cast<uint64_t>(descriptor_defs::Flag::Writable) |
                         static_cast<uint64_t>(descriptor_defs::Flag::Async);
        long handle = pipe_open_existing(flags, request.reply_pipe_id);
        if (handle >= 0) {
            Listener temp{};
            temp.port = request.port;
            temp.app_pipe_handle = static_cast<uint32_t>(handle);
            send_listen_response(temp, tcpd_protocol::kStatusInUse);
            descriptor_close(static_cast<uint32_t>(handle));
        }
        return;
    }

    Listener* listener = allocate_listener(ctx);
    if (listener == nullptr) {
        return;
    }

    uint64_t flags = static_cast<uint64_t>(descriptor_defs::Flag::Writable) |
                     static_cast<uint64_t>(descriptor_defs::Flag::Async);
    long handle = pipe_open_existing(flags, request.reply_pipe_id);
    if (handle < 0) {
        return;
    }

    listener->in_use = true;
    listener->bound = false;
    listener->port = request.port;
    listener->app_pipe_id = request.reply_pipe_id;
    listener->app_pipe_handle = static_cast<uint32_t>(handle);

    networkd_protocol::Message message{};
    networkd_protocol::init_message(message, networkd_protocol::kBindTcpRequest);
    message.bind_tcp_request.reply_pipe_id = ctx.network_reply_pipe_id;
    message.bind_tcp_request.port = request.port;
    if (!networkd_protocol::write_message(ctx.networkd_server_pipe, message)) {
        send_listen_response(*listener, tcpd_protocol::kStatusIo);
        descriptor_close(listener->app_pipe_handle);
        listener->in_use = false;
    }
}

void handle_send_request(ServerContext& ctx, const tcpd_protocol::SendRequest& request) {
    if (request.payload_length > tcpd_protocol::kMaxPayload) {
        return;
    }
    Connection* conn = find_connection(ctx, request.connection_id);
    if (conn == nullptr || conn->state != kConnStateEstablished) {
        return;
    }
    descriptor_defs::NetIpv4Config cfg{};
    if (!load_ipv4_config(ctx, cfg)) {
        return;
    }
    if (send_network_segment(ctx,
                             cfg.address,
                             conn->remote_ip,
                             conn->local_port,
                             conn->remote_port,
                             conn->local_next_seq,
                             conn->remote_next_seq,
                             usernet::kTcpFlagAck |
                                 (request.payload_length != 0 ? usernet::kTcpFlagPsh : 0),
                             request.payload,
                             request.payload_length)) {
        conn->local_next_seq += request.payload_length;
    }
}

void handle_close_request(ServerContext& ctx, const tcpd_protocol::CloseRequest& request) {
    Connection* conn = find_connection(ctx, request.connection_id);
    if (conn == nullptr || conn->state != kConnStateEstablished) {
        return;
    }
    descriptor_defs::NetIpv4Config cfg{};
    if (!load_ipv4_config(ctx, cfg)) {
        return;
    }
    (void)send_network_segment(ctx,
                               cfg.address,
                               conn->remote_ip,
                               conn->local_port,
                               conn->remote_port,
                               conn->local_next_seq,
                               conn->remote_next_seq,
                               usernet::kTcpFlagFin | usernet::kTcpFlagAck,
                               nullptr,
                               0);
    conn->local_next_seq += 1;
    close_connection(ctx, *conn, tcpd_protocol::kCloseLocalClose);
}

void handle_connect_request(ServerContext& ctx,
                            const tcpd_protocol::ConnectRequest& request) {
    if (request.reply_pipe_id == 0 || request.remote_port == 0) {
        return;
    }

    uint64_t flags = static_cast<uint64_t>(descriptor_defs::Flag::Writable) |
                     static_cast<uint64_t>(descriptor_defs::Flag::Async);
    long handle = pipe_open_existing(flags, request.reply_pipe_id);
    if (handle < 0) {
        return;
    }
    uint32_t app_pipe_handle = static_cast<uint32_t>(handle);

    uint16_t local_port = choose_client_port(ctx);
    if (local_port == 0) {
        send_connect_response(app_pipe_handle, tcpd_protocol::kStatusIo, 0, 0);
        descriptor_close(app_pipe_handle);
        return;
    }

    ClientPort* client_port = find_client_port(ctx, local_port);
    if (client_port != nullptr && client_port->bound) {
        if (!begin_outbound_connection(ctx,
                                       app_pipe_handle,
                                       local_port,
                                       request.remote_ip,
                                       request.remote_port)) {
            send_connect_response(app_pipe_handle, tcpd_protocol::kStatusIo, 0, local_port);
            descriptor_close(app_pipe_handle);
        }
        return;
    }

    if (client_port == nullptr) {
        client_port = allocate_client_port(ctx);
    }
    PendingConnect* pending = allocate_pending_connect(ctx);
    if (client_port == nullptr || pending == nullptr) {
        send_connect_response(app_pipe_handle, tcpd_protocol::kStatusIo, 0, 0);
        descriptor_close(app_pipe_handle);
        return;
    }

    client_port->in_use = true;
    client_port->bound = false;
    client_port->binding_in_progress = true;
    client_port->port = local_port;

    pending->in_use = true;
    pending->app_pipe_handle = app_pipe_handle;
    pending->local_port = local_port;
    pending->remote_port = request.remote_port;
    for (size_t i = 0; i < 4; ++i) {
        pending->remote_ip[i] = request.remote_ip[i];
    }

    networkd_protocol::Message message{};
    networkd_protocol::init_message(message, networkd_protocol::kBindTcpRequest);
    message.bind_tcp_request.reply_pipe_id = ctx.network_reply_pipe_id;
    message.bind_tcp_request.port = local_port;
    if (!networkd_protocol::write_message(ctx.networkd_server_pipe, message)) {
        pending->in_use = false;
        client_port->binding_in_progress = false;
        send_connect_response(app_pipe_handle, tcpd_protocol::kStatusIo, 0, local_port);
        descriptor_close(app_pipe_handle);
    }
}

void poll_control(ServerContext& ctx) {
    for (;;) {
        tcpd_protocol::Message message{};
        if (!tcpd_protocol::read_message(ctx.tcpd_server_pipe, message)) {
            break;
        }
        if (message.type == tcpd_protocol::kListenRequest) {
            handle_listen_request(ctx, message.listen_request);
        } else if (message.type == tcpd_protocol::kSendRequest) {
            handle_send_request(ctx, message.send_request);
        } else if (message.type == tcpd_protocol::kCloseRequest) {
            handle_close_request(ctx, message.close_request);
        } else if (message.type == tcpd_protocol::kConnectRequest) {
            handle_connect_request(ctx, message.connect_request);
        }
    }
}

void handle_bind_response(ServerContext& ctx,
                          const networkd_protocol::BindTcpResponse& response) {
    Listener* listener = find_listener(ctx, response.port);
    if (listener != nullptr) {
        listener->bound = (response.status == networkd_protocol::kStatusOk);
        send_listen_response(*listener,
                             listener->bound ? tcpd_protocol::kStatusOk
                                             : tcpd_protocol::kStatusInUse);
        if (!listener->bound) {
            descriptor_close(listener->app_pipe_handle);
            listener->in_use = false;
        }
        recount_registry(ctx);
        return;
    }

    PendingConnect* pending = find_pending_connect(ctx, response.port);
    ClientPort* client_port = find_client_port(ctx, response.port);
    if (pending == nullptr || client_port == nullptr) {
        return;
    }
    client_port->binding_in_progress = false;
    client_port->bound = (response.status == networkd_protocol::kStatusOk);
    if (!client_port->bound) {
        send_connect_response(pending->app_pipe_handle, tcpd_protocol::kStatusInUse, 0, 0);
        descriptor_close(pending->app_pipe_handle);
        pending->in_use = false;
        client_port->in_use = false;
        client_port->port = 0;
        return;
    }

    if (!begin_outbound_connection(ctx,
                                   pending->app_pipe_handle,
                                   pending->local_port,
                                   pending->remote_ip,
                                   pending->remote_port)) {
        send_connect_response(pending->app_pipe_handle,
                              tcpd_protocol::kStatusIo,
                              0,
                              pending->local_port);
        descriptor_close(pending->app_pipe_handle);
    }
    pending->in_use = false;
}

void send_ack_only(ServerContext& ctx,
                   Connection& conn,
                   uint16_t flags = usernet::kTcpFlagAck) {
    descriptor_defs::NetIpv4Config cfg{};
    if (!load_ipv4_config(ctx, cfg)) {
        return;
    }
    (void)send_network_segment(ctx,
                               cfg.address,
                               conn.remote_ip,
                               conn.local_port,
                               conn.remote_port,
                               conn.local_next_seq,
                               conn.remote_next_seq,
                               flags,
                               nullptr,
                               0);
}

void handle_new_connection(ServerContext& ctx,
                           Listener& listener,
                           const networkd_protocol::TcpSegmentEvent& segment) {
    if ((segment.flags & usernet::kTcpFlagSyn) == 0 ||
        (segment.flags & usernet::kTcpFlagAck) != 0) {
        return;
    }
    Connection* conn = allocate_connection(ctx);
    if (conn == nullptr) {
        return;
    }
    conn->in_use = true;
    conn->id = ++ctx.next_connection_id;
    conn->state = kConnStateSynReceived;
    for (size_t i = 0; i < 4; ++i) {
        conn->remote_ip[i] = segment.source_ip[i];
    }
    conn->local_port = segment.destination_port;
    conn->remote_port = segment.source_port;
    conn->remote_next_seq = segment.sequence_number + 1;
    uint32_t initial_seq =
        0x4E540000u + (static_cast<uint32_t>(listener.port) << 4) + conn->id;
    conn->local_next_seq = initial_seq + 1;
    conn->listener = &listener;

    descriptor_defs::NetIpv4Config cfg{};
    if (!load_ipv4_config(ctx, cfg)) {
        reset_connection(*conn);
        return;
    }
    if (!send_network_segment(ctx,
                              cfg.address,
                              conn->remote_ip,
                              conn->local_port,
                              conn->remote_port,
                              initial_seq,
                              conn->remote_next_seq,
                              usernet::kTcpFlagSyn | usernet::kTcpFlagAck,
                              nullptr,
                              0)) {
        reset_connection(*conn);
        return;
    }
    recount_registry(ctx);
}

void handle_existing_connection(ServerContext& ctx,
                                Connection& conn,
                                const networkd_protocol::TcpSegmentEvent& segment) {
    if ((segment.flags & usernet::kTcpFlagRst) != 0) {
        close_connection(ctx, conn, tcpd_protocol::kCloseRemoteRst);
        return;
    }

    size_t payload_length = segment.payload_length;
    uint32_t seq = segment.sequence_number;

    if (conn.state == kConnStateSynReceived) {
        if ((segment.flags & usernet::kTcpFlagAck) != 0 &&
            segment.acknowledgment_number == conn.local_next_seq &&
            seq == conn.remote_next_seq) {
            conn.state = kConnStateEstablished;
            send_accept_event(conn);
        }
        return;
    }

    if (conn.state == kConnStateSynSent) {
        if ((segment.flags & usernet::kTcpFlagRst) != 0) {
            send_connect_response(conn.app_pipe_handle,
                                  tcpd_protocol::kStatusNotFound,
                                  0,
                                  conn.local_port);
            descriptor_close(conn.app_pipe_handle);
            reset_connection(conn);
            recount_registry(ctx);
            return;
        }
        if ((segment.flags & usernet::kTcpFlagSyn) != 0 &&
            (segment.flags & usernet::kTcpFlagAck) != 0 &&
            segment.acknowledgment_number == conn.local_next_seq) {
            conn.remote_next_seq = seq + 1;
            conn.state = kConnStateEstablished;
            send_ack_only(ctx, conn);
            send_connect_response(conn.app_pipe_handle,
                                  tcpd_protocol::kStatusOk,
                                  conn.id,
                                  conn.local_port);
        }
        return;
    }

    if (conn.state != kConnStateEstablished) {
        return;
    }

    if (seq != conn.remote_next_seq) {
        send_ack_only(ctx, conn);
        return;
    }

    if (payload_length != 0) {
        conn.remote_next_seq += static_cast<uint32_t>(payload_length);
        send_data_event(conn, segment.payload, payload_length);
    }

    if ((segment.flags & usernet::kTcpFlagFin) != 0) {
        conn.remote_next_seq += 1;
        send_ack_only(ctx, conn);
        close_connection(ctx, conn, tcpd_protocol::kCloseRemoteFin);
        return;
    }

    if (payload_length != 0) {
        send_ack_only(ctx, conn);
    }
}

void handle_tcp_segment(ServerContext& ctx,
                        const networkd_protocol::TcpSegmentEvent& segment) {
    if (ctx.registry != nullptr) {
        ++ctx.registry->rx_segments;
    }
    Connection* conn = find_connection_by_tuple(ctx,
                                                segment.destination_port,
                                                segment.source_ip,
                                                segment.source_port);
    if (conn != nullptr) {
        handle_existing_connection(ctx, *conn, segment);
        return;
    }
    Listener* listener = find_listener(ctx, segment.destination_port);
    if (listener == nullptr || !listener->bound) {
        return;
    }
    handle_new_connection(ctx, *listener, segment);
}

void poll_network(ServerContext& ctx) {
    for (;;) {
        networkd_protocol::Message message{};
        if (!networkd_protocol::read_message(ctx.network_reply_pipe, message)) {
            break;
        }
        if (message.type == networkd_protocol::kBindTcpResponse) {
            handle_bind_response(ctx, message.bind_tcp_response);
        } else if (message.type == networkd_protocol::kTcpSegmentEvent) {
            handle_tcp_segment(ctx, message.tcp_event);
        }
    }
}

bool wait_for_networkd(uint32_t& server_pipe_id) {
    uint32_t registry_handle = 0;
    networkd_protocol::Registry* registry = nullptr;
    long shm = shared_memory_open(networkd_protocol::kRegistryName,
                                  sizeof(networkd_protocol::Registry));
    if (shm < 0) {
        return false;
    }
    descriptor_defs::SharedMemoryInfo info{};
    if (shared_memory_get_info(static_cast<uint32_t>(shm), &info) != 0 ||
        info.base == 0 ||
        info.length < sizeof(networkd_protocol::Registry)) {
        descriptor_close(static_cast<uint32_t>(shm));
        return false;
    }
    registry_handle = static_cast<uint32_t>(shm);
    registry = reinterpret_cast<networkd_protocol::Registry*>(info.base);
    while (registry->magic != networkd_protocol::kRegistryMagic ||
           registry->version != networkd_protocol::kRegistryVersion ||
           registry->server_pipe_id == 0) {
        yield();
    }
    server_pipe_id = registry->server_pipe_id;
    descriptor_close(registry_handle);
    return true;
}

}  // namespace

int main(uint64_t, uint64_t) {
    ServerContext ctx{};

    if (!usernet::open_device(ctx.device, 0, static_cast<uint64_t>(descriptor_defs::Flag::Async))) {
        print_line("tcpd: failed to open net device 0");
        return 1;
    }

    uint64_t server_flags = static_cast<uint64_t>(descriptor_defs::Flag::Readable) |
                            static_cast<uint64_t>(descriptor_defs::Flag::Async);
    long tcpd_server_pipe = pipe_open_new(server_flags);
    if (tcpd_server_pipe < 0) {
        print_line("tcpd: failed to create server pipe");
        return 1;
    }
    ctx.tcpd_server_pipe = static_cast<uint32_t>(tcpd_server_pipe);

    descriptor_defs::PipeInfo info{};
    if (pipe_get_info(ctx.tcpd_server_pipe, &info) != 0 || info.id == 0) {
        print_line("tcpd: failed to query server pipe");
        return 1;
    }
    if (!populate_registry(ctx, info.id)) {
        print_line("tcpd: failed to publish registry");
        return 1;
    }

    long network_reply_pipe = pipe_open_new(server_flags);
    if (network_reply_pipe < 0) {
        print_line("tcpd: failed to create network reply pipe");
        return 1;
    }
    ctx.network_reply_pipe = static_cast<uint32_t>(network_reply_pipe);
    if (pipe_get_info(ctx.network_reply_pipe, &info) != 0 || info.id == 0) {
        print_line("tcpd: failed to query network reply pipe");
        return 1;
    }
    ctx.network_reply_pipe_id = info.id;

    uint32_t networkd_server_pipe_id = 0;
    if (!wait_for_networkd(networkd_server_pipe_id)) {
        print_line("tcpd: failed to locate networkd");
        return 1;
    }

    uint64_t networkd_flags = static_cast<uint64_t>(descriptor_defs::Flag::Writable) |
                              static_cast<uint64_t>(descriptor_defs::Flag::Async);
    long networkd_server =
        pipe_open_existing(networkd_flags, networkd_server_pipe_id);
    if (networkd_server < 0) {
        print_line("tcpd: failed to open networkd pipe");
        return 1;
    }
    ctx.networkd_server_pipe = static_cast<uint32_t>(networkd_server);
    if (ctx.registry != nullptr) {
        ctx.registry->state = tcpd_protocol::kStateReady;
    }

    print_line("tcpd: ready");

    for (;;) {
        poll_control(ctx);
        poll_network(ctx);
        yield();
    }
}
