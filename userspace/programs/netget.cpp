#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../crt/syscall.hpp"
#include "../helpers/http.hpp"
#include "../net/dns.hpp"
#include "../net/tcpd_protocol.hpp"

namespace {

constexpr uint32_t kConnectWaitLimit = 200000;

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
    size_t len = strlen(text);
    if (len != 0) {
        descriptor_write(static_cast<uint32_t>(console), text, len);
    }
}

void print_line(const char* text) {
    print(text);
    print("\n");
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

bool write_tcpd_message(uint32_t handle, const tcpd_protocol::Message& message) {
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&message);
    size_t written = 0;
    while (written < sizeof(message)) {
        long result = descriptor_write(handle, bytes + written, sizeof(message) - written);
        if (result == 0 || result == kDescriptorWouldBlock) {
            yield();
            continue;
        }
        if (result <= 0) {
            return false;
        }
        written += static_cast<size_t>(result);
    }
    return true;
}

bool send_connect_request(uint32_t server_handle,
                          uint32_t reply_pipe_id,
                          uint32_t endpoint_id,
                          const uint8_t ip[4],
                          uint16_t port) {
    tcpd_protocol::Message message{};
    tcpd_protocol::init_message(message, tcpd_protocol::kConnectRequest);
    message.connect_request.reply_pipe_id = reply_pipe_id;
    message.connect_request.remote_port = port;
    message.connect_request.endpoint_id = endpoint_id;
    for (size_t i = 0; i < 4; ++i) {
        message.connect_request.remote_ip[i] = ip[i];
    }
    return write_tcpd_message(server_handle, message);
}

bool append_bytes(char* dest,
                  size_t capacity,
                  size_t& length,
                  const char* begin,
                  const char* end) {
    if (dest == nullptr || begin == nullptr || end == nullptr || begin > end) {
        return false;
    }
    size_t count = static_cast<size_t>(end - begin);
    if (length + count > capacity) {
        return false;
    }
    for (size_t i = 0; i < count; ++i) {
        dest[length + i] = begin[i];
    }
    length += count;
    return true;
}

bool append_string(char* dest, size_t capacity, size_t& length, const char* text) {
    return append_bytes(dest, capacity, length, text, text + strlen(text));
}

bool write_console(const uint8_t* bytes, size_t length) {
    static int32_t console = -1;
    if (console < 0) {
        console = static_cast<int32_t>(
            descriptor_open(static_cast<uint32_t>(descriptor_defs::Type::Console), 0));
        if (console < 0) {
            return false;
        }
    }
    size_t offset = 0;
    while (offset < length) {
        long written =
            descriptor_write(static_cast<uint32_t>(console), bytes + offset, length - offset);
        if (written <= 0) {
            return false;
        }
        offset += static_cast<size_t>(written);
    }
    return true;
}

}  // namespace

int main(uint64_t arg_ptr, uint64_t) {
    const char* args = reinterpret_cast<const char*>(arg_ptr);
    userspace::http::Url target{};
    if (!userspace::http::parse_url(args,
                                    target,
                                    userspace::http::UrlParseMode::DefaultHttp)) {
        print_line("usage: netget <host[:port][/path]>");
        return 1;
    }
    if (target.scheme != userspace::http::Scheme::Http) {
        print_line("netget: https is not supported; use download for TLS");
        return 1;
    }

    uint8_t target_ip[4];
    if (!userspace::http::parse_ipv4_literal(target.host, target_ip)) {
        if (!usernet::dns::resolve_a(target.host, target_ip)) {
            print_line("netget: dns lookup failed");
            return 1;
        }
    }

    uint32_t registry_handle = 0;
    tcpd_protocol::Registry* registry = nullptr;
    if (!open_tcpd_registry(registry_handle, registry)) {
        print_line("netget: failed to open tcpd registry");
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
        print_line("netget: failed to create reply pipe");
        return 1;
    }
    descriptor_defs::PipeInfo reply_info{};
    if (pipe_get_info(static_cast<uint32_t>(reply_pipe), &reply_info) != 0 ||
        reply_info.id == 0) {
        print_line("netget: failed to query reply pipe");
        return 1;
    }

    uint64_t server_flags = static_cast<uint64_t>(descriptor_defs::Flag::Writable) |
                            static_cast<uint64_t>(descriptor_defs::Flag::Async);
    long server_pipe = pipe_open_existing(server_flags, registry->server_pipe_id);
    if (server_pipe < 0) {
        print_line("netget: failed to open tcpd pipe");
        return 1;
    }

    long endpoint = net_endpoint_open_new(
        static_cast<uint64_t>(descriptor_defs::Flag::Async));
    if (endpoint < 0) {
        print_line("netget: failed to create endpoint");
        return 1;
    }
    descriptor_defs::NetEndpointInfo endpoint_info{};
    if (net_endpoint_get_info(static_cast<uint32_t>(endpoint), &endpoint_info) != 0 ||
        endpoint_info.id == 0) {
        print_line("netget: failed to query endpoint");
        return 1;
    }

    if (!send_connect_request(static_cast<uint32_t>(server_pipe),
                              reply_info.id,
                              endpoint_info.id,
                              target_ip,
                              target.port)) {
        print_line("netget: failed to send connect request");
        return 1;
    }

    uint32_t connection_id = 0;
    uint32_t connect_waits = 0;
    for (;;) {
        tcpd_protocol::Message message{};
        if (!tcpd_protocol::read_message(static_cast<uint32_t>(reply_pipe), message)) {
            if (connect_waits++ >= kConnectWaitLimit) {
                print_line("netget: connect timeout");
                return 1;
            }
            yield();
            continue;
        }

        if (message.type == tcpd_protocol::kConnectResponse) {
            if (message.connect_response.status != tcpd_protocol::kStatusOk) {
                print_line("netget: connect failed");
                return 1;
            }
            connection_id = message.connect_response.connection_id;
            break;
        }
    }

    char request[tcpd_protocol::kMaxPayload];
    size_t request_length = 0;
    const char request_prefix[] = "GET ";
    const char request_middle[] = " HTTP/1.0\r\nHost: ";
    const char request_suffix[] = "\r\nConnection: close\r\n\r\n";
    if (!append_string(request, sizeof(request), request_length, request_prefix) ||
        !append_string(request, sizeof(request), request_length, target.path) ||
        !append_string(request, sizeof(request), request_length, request_middle) ||
        !append_string(request, sizeof(request), request_length, target.host) ||
        !append_string(request, sizeof(request), request_length, request_suffix)) {
        print_line("netget: request too long");
        return 1;
    }
    (void)connection_id;
    size_t sent = 0;
    while (sent < request_length) {
        long result = descriptor_write(static_cast<uint32_t>(endpoint),
                                       request + sent,
                                       request_length - sent);
        if (result == kDescriptorWouldBlock) {
            yield();
            continue;
        }
        if (result <= 0) {
            break;
        }
        sent += static_cast<size_t>(result);
    }
    if (sent != request_length) {
        print_line("netget: failed to send request");
        return 1;
    }

    uint8_t response[1024];
    for (;;) {
        long read = descriptor_read(static_cast<uint32_t>(endpoint),
                                    response,
                                    sizeof(response));
        if (read == kDescriptorWouldBlock) {
            yield();
            continue;
        }
        if (read == 0) {
            return 0;
        }
        if (read < 0) {
            print_line("netget: endpoint read failed");
            return 1;
        }
        if (!write_console(response, static_cast<size_t>(read))) {
            return 1;
        }
    }
}
