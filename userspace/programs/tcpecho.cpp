#include <stddef.h>
#include <stdint.h>

#include "../crt/syscall.hpp"
#include "../net/tcpd_protocol.hpp"

namespace {

constexpr size_t kMaxEchoConnections = 16;

struct EchoConnection {
    bool in_use;
    uint32_t id;
    uint32_t endpoint;
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

bool parse_u16(const char* text, uint16_t& out) {
    if (text == nullptr || *text == '\0') {
        return false;
    }
    uint32_t value = 0;
    while (*text != '\0') {
        if (*text < '0' || *text > '9') {
            return false;
        }
        value = value * 10u + static_cast<uint32_t>(*text - '0');
        if (value > 65535u) {
            return false;
        }
        ++text;
    }
    out = static_cast<uint16_t>(value);
    return true;
}

bool open_tcpd_registry(uint32_t& handle, tcpd_protocol::Registry*& registry) {
    long shm = shared_memory_open(tcpd_protocol::kRegistryName,
                                  sizeof(tcpd_protocol::Registry));
    if (shm < 0) {
        return false;
    }
    descriptor_defs::SharedMemoryInfo info{};
    if (shared_memory_get_info(static_cast<uint32_t>(shm), &info) != 0 ||
        info.base == 0 ||
        info.length < sizeof(tcpd_protocol::Registry)) {
        descriptor_close(static_cast<uint32_t>(shm));
        return false;
    }
    handle = static_cast<uint32_t>(shm);
    registry = reinterpret_cast<tcpd_protocol::Registry*>(info.base);
    return true;
}

bool send_listen_request(uint32_t server_handle,
                         uint32_t reply_pipe_id,
                         uint16_t port) {
    tcpd_protocol::Message message{};
    tcpd_protocol::init_message(message, tcpd_protocol::kListenRequest);
    message.listen_request.reply_pipe_id = reply_pipe_id;
    message.listen_request.port = port;
    return tcpd_protocol::write_message(server_handle, message);
}

bool send_echo(uint32_t server_handle,
               uint32_t connection_id,
               const uint8_t* payload,
               size_t payload_length) {
    if (payload_length > tcpd_protocol::kMaxPayload) {
        return false;
    }
    tcpd_protocol::Message message{};
    tcpd_protocol::init_message(message, tcpd_protocol::kSendRequest);
    message.send_request.connection_id = connection_id;
    message.send_request.payload_length = static_cast<uint16_t>(payload_length);
    for (size_t i = 0; i < payload_length; ++i) {
        message.send_request.payload[i] = payload[i];
    }
    return tcpd_protocol::write_message(server_handle, message);
}

EchoConnection* find_connection(EchoConnection* connections, uint32_t id) {
    for (size_t i = 0; i < kMaxEchoConnections; ++i) {
        if (connections[i].in_use && connections[i].id == id) {
            return &connections[i];
        }
    }
    return nullptr;
}

EchoConnection* allocate_connection(EchoConnection* connections) {
    for (size_t i = 0; i < kMaxEchoConnections; ++i) {
        if (!connections[i].in_use) {
            return &connections[i];
        }
    }
    return nullptr;
}

void close_echo_connection(EchoConnection& connection) {
    if (connection.endpoint != 0) {
        descriptor_close(connection.endpoint);
    }
    connection.in_use = false;
    connection.id = 0;
    connection.endpoint = 0;
}

void poll_echo_endpoints(EchoConnection* connections) {
    uint8_t buffer[tcpd_protocol::kMaxPayload];
    for (size_t i = 0; i < kMaxEchoConnections; ++i) {
        EchoConnection& connection = connections[i];
        if (!connection.in_use || connection.endpoint == 0) {
            continue;
        }
        for (;;) {
            long read = descriptor_read(connection.endpoint, buffer, sizeof(buffer));
            if (read == kDescriptorWouldBlock) {
                break;
            }
            if (read <= 0) {
                close_echo_connection(connection);
                break;
            }
            size_t written = 0;
            while (written < static_cast<size_t>(read)) {
                long result = descriptor_write(connection.endpoint,
                                               buffer + written,
                                               static_cast<size_t>(read) - written);
                if (result == kDescriptorWouldBlock) {
                    yield();
                    continue;
                }
                if (result <= 0) {
                    close_echo_connection(connection);
                    break;
                }
                written += static_cast<size_t>(result);
            }
            if (!connection.in_use) {
                break;
            }
        }
    }
}

}  // namespace

int main(uint64_t arg_ptr, uint64_t) {
    const char* args = reinterpret_cast<const char*>(arg_ptr);
    uint16_t port = 8080;
    if (args != nullptr && args[0] != '\0' && !parse_u16(args, port)) {
        print_line("tcpecho: invalid port");
        return 1;
    }

    uint32_t registry_handle = 0;
    tcpd_protocol::Registry* registry = nullptr;
    if (!open_tcpd_registry(registry_handle, registry)) {
        print_line("tcpecho: failed to open tcpd registry");
        return 1;
    }
    while (registry->magic != tcpd_protocol::kRegistryMagic ||
           registry->version != tcpd_protocol::kRegistryVersion ||
           registry->server_pipe_id == 0) {
        yield();
    }

    uint64_t reply_flags = static_cast<uint64_t>(descriptor_defs::Flag::Readable) |
                           static_cast<uint64_t>(descriptor_defs::Flag::Async);
    long reply_pipe = pipe_open_new(reply_flags);
    if (reply_pipe < 0) {
        print_line("tcpecho: failed to create reply pipe");
        return 1;
    }
    descriptor_defs::PipeInfo info{};
    if (pipe_get_info(static_cast<uint32_t>(reply_pipe), &info) != 0 || info.id == 0) {
        print_line("tcpecho: failed to query reply pipe");
        return 1;
    }

    uint64_t server_flags = static_cast<uint64_t>(descriptor_defs::Flag::Writable) |
                            static_cast<uint64_t>(descriptor_defs::Flag::Async);
    long server_pipe = pipe_open_existing(server_flags, registry->server_pipe_id);
    if (server_pipe < 0) {
        print_line("tcpecho: failed to open tcpd pipe");
        return 1;
    }
    if (!send_listen_request(static_cast<uint32_t>(server_pipe), info.id, port)) {
        print_line("tcpecho: failed to send listen request");
        return 1;
    }

    tcpd_protocol::Message message{};
    while (!tcpd_protocol::read_message(static_cast<uint32_t>(reply_pipe), message)) {
        yield();
    }
    if (message.type != tcpd_protocol::kListenResponse ||
        message.listen_response.status != tcpd_protocol::kStatusOk) {
        print_line("tcpecho: listen failed");
        return 1;
    }

    print("tcpecho: listening on ");
    print_u32(port);
    print("\n");

    EchoConnection connections[kMaxEchoConnections]{};
    for (;;) {
        poll_echo_endpoints(connections);
        tcpd_protocol::Message event{};
        if (!tcpd_protocol::read_message(static_cast<uint32_t>(reply_pipe), event)) {
            yield();
            continue;
        }
        if (event.type == tcpd_protocol::kAcceptEvent) {
            print("tcpecho: accept ");
            print_u32(event.accept_event.connection_id);
            print("\n");
            EchoConnection* connection = allocate_connection(connections);
            if (connection != nullptr && event.accept_event.endpoint_id != 0) {
                long endpoint = net_endpoint_open_existing(
                    static_cast<uint64_t>(descriptor_defs::Flag::Async),
                    event.accept_event.endpoint_id);
                if (endpoint >= 0) {
                    connection->in_use = true;
                    connection->id = event.accept_event.connection_id;
                    connection->endpoint = static_cast<uint32_t>(endpoint);
                }
            }
            continue;
        }
        if (event.type == tcpd_protocol::kDataEvent) {
            if (find_connection(connections, event.data_event.connection_id) != nullptr) {
                continue;
            }
            (void)send_echo(static_cast<uint32_t>(server_pipe),
                            event.data_event.connection_id,
                            event.data_event.payload,
                            event.data_event.payload_length);
            continue;
        }
        if (event.type == tcpd_protocol::kClosedEvent) {
            print("tcpecho: closed ");
            print_u32(event.closed_event.connection_id);
            print("\n");
            EchoConnection* connection =
                find_connection(connections, event.closed_event.connection_id);
            if (connection != nullptr) {
                close_echo_connection(*connection);
            }
        }
    }
}
