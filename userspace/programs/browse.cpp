#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <bearssl.h>

#include "descriptors.hpp"
#include "keyboard_scancode.hpp"
#include "../crt/syscall.hpp"
#include "../net/dns.hpp"
#include "../net/tcpd_protocol.hpp"

namespace {

constexpr uint32_t kDescConsole =
    static_cast<uint32_t>(descriptor_defs::Type::Console);
constexpr uint32_t kDescKeyboard =
    static_cast<uint32_t>(descriptor_defs::Type::Keyboard);
constexpr size_t kMaxUrl = 512;
constexpr size_t kMaxHost = 256;
constexpr size_t kMaxPath = 512;
constexpr size_t kMaxHeader = 8192;
constexpr size_t kMaxLocation = 512;
constexpr size_t kIoBufferSize = 1024;
constexpr size_t kRenderWidth = 78;
constexpr uint32_t kConnectWaitLimit = 200000;
constexpr uint32_t kMaxRedirects = 4;

enum class Scheme : uint8_t {
    Http,
    Https,
};

enum class TlsMode : uint8_t {
    None,
    Insecure,
    VerifiedNoTime,
};

struct Url {
    Scheme scheme;
    char host[kMaxHost];
    char path[kMaxPath];
    uint16_t port;
};

struct TcpConnection {
    uint32_t server_pipe = 0;
    uint32_t reply_pipe = 0;
    uint32_t connection_id = 0;
    bool closed = false;
    uint8_t pending[tcpd_protocol::kMaxPayload];
    size_t pending_offset = 0;
    size_t pending_length = 0;
};

struct InsecureX509Context {
    const br_x509_class* vtable;
    br_x509_decoder_context decoder;
    unsigned cert_count;
    unsigned end_error;
    unsigned usages;
    bool have_pkey;
};

struct Buffer {
    uint8_t* data;
    size_t size;
    size_t capacity;
};

struct TrustStore {
    bool attempted;
    bool loaded;
    br_x509_trust_anchor* anchors;
    size_t anchor_count;
    size_t anchor_capacity;
};

struct HttpResponseMeta {
    int status_code;
    bool chunked;
    bool have_content_length;
    size_t content_length;
    bool is_html;
    bool is_text;
    char location[kMaxLocation];
};

struct HtmlRenderer {
    bool in_tag;
    bool in_entity;
    bool in_comment;
    bool in_script;
    bool in_style;
    bool pending_space;
    bool last_was_newline;
    bool have_visible_text;
    char tag_buffer[96];
    size_t tag_length;
    char entity_buffer[16];
    size_t entity_length;
    size_t column;
};

struct Stream {
    TcpConnection* tcp;
    bool tls;
    TlsMode tls_mode;
    br_ssl_client_context ssl_client;
    br_sslio_context ssl_io;
    uint8_t ssl_iobuf[BR_SSL_BUFSIZE_BIDI];
    br_x509_minimal_context x509_minimal;
    InsecureX509Context insecure_x509;
};

long g_console = -1;
TrustStore* g_trust_store = reinterpret_cast<TrustStore*>(1);

void print_raw(const void* data, size_t length) {
    if (g_console < 0 || data == nullptr || length == 0) {
        return;
    }
    descriptor_write(static_cast<uint32_t>(g_console), data, length);
}

void print(const char* text) {
    if (text == nullptr) {
        return;
    }
    print_raw(text, strlen(text));
}

void print_line(const char* text) {
    print(text);
    print("\n");
}

bool ascii_is_digit(char ch) {
    return ch >= '0' && ch <= '9';
}

char ascii_to_lower(char ch) {
    if (ch >= 'A' && ch <= 'Z') {
        return static_cast<char>(ch - 'A' + 'a');
    }
    return ch;
}

bool ascii_starts_with_case_insensitive(const char* text, const char* prefix) {
    if (text == nullptr || prefix == nullptr) {
        return false;
    }
    while (*prefix != '\0') {
        if (*text == '\0' || ascii_to_lower(*text) != ascii_to_lower(*prefix)) {
            return false;
        }
        ++text;
        ++prefix;
    }
    return true;
}

int ascii_find_char(const char* text, char ch) {
    if (text == nullptr) {
        return -1;
    }
    for (size_t i = 0; text[i] != '\0'; ++i) {
        if (text[i] == ch) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void copy_range(char* dest, size_t capacity, const char* begin, const char* end) {
    if (dest == nullptr || capacity == 0) {
        return;
    }
    size_t length = 0;
    if (begin != nullptr && end != nullptr && begin <= end) {
        length = static_cast<size_t>(end - begin);
        if (length >= capacity) {
            length = capacity - 1;
        }
        if (length != 0) {
            memcpy(dest, begin, length);
        }
    }
    dest[length] = '\0';
}

bool buffer_reserve(Buffer& buffer, size_t required) {
    if (required <= buffer.capacity) {
        return true;
    }
    size_t new_capacity = buffer.capacity == 0 ? 1024 : buffer.capacity;
    while (new_capacity < required) {
        if (new_capacity > (static_cast<size_t>(-1) / 2)) {
            return false;
        }
        new_capacity *= 2;
    }
    auto* new_data = static_cast<uint8_t*>(map_anonymous(new_capacity, MAP_WRITE));
    if (new_data == nullptr) {
        return false;
    }
    if (buffer.size != 0 && buffer.data != nullptr) {
        memcpy(new_data, buffer.data, buffer.size);
    }
    buffer.data = new_data;
    buffer.capacity = new_capacity;
    return true;
}

bool buffer_append(Buffer& buffer, const void* data, size_t length) {
    if (length == 0) {
        return true;
    }
    if (!buffer_reserve(buffer, buffer.size + length)) {
        return false;
    }
    memcpy(buffer.data + buffer.size, data, length);
    buffer.size += length;
    return true;
}

void buffer_clear(Buffer& buffer) {
    buffer.size = 0;
}

void* alloc_persistent_bytes(size_t size) {
    if (size == 0) {
        size = 1;
    }
    return map_anonymous(size, MAP_WRITE);
}

bool load_file(const char* path, Buffer& out) {
    print("browse: load_file ");
    print_line(path);
    long handle = file_open(path);
    if (handle < 0) {
        print_line("browse: load_file open failed");
        return false;
    }
    buffer_clear(out);
    uint8_t temp[512];
    while (true) {
        long read = file_read(static_cast<uint32_t>(handle), temp, sizeof(temp));
        if (read < 0) {
            file_close(static_cast<uint32_t>(handle));
            print_line("browse: load_file read failed");
            return false;
        }
        if (read == 0) {
            break;
        }
        if (!buffer_append(out, temp, static_cast<size_t>(read))) {
            file_close(static_cast<uint32_t>(handle));
            print_line("browse: load_file append failed");
            return false;
        }
    }
    file_close(static_cast<uint32_t>(handle));
    print_line("browse: load_file ok");
    return true;
}

bool trust_store_reserve(TrustStore& store, size_t required) {
    if (required <= store.anchor_capacity) {
        return true;
    }
    size_t new_capacity = store.anchor_capacity == 0 ? 8 : store.anchor_capacity;
    while (new_capacity < required) {
        if (new_capacity > (static_cast<size_t>(-1) / 2)) {
            return false;
        }
        new_capacity *= 2;
    }
    auto* new_anchors = static_cast<br_x509_trust_anchor*>(
        map_anonymous(new_capacity * sizeof(br_x509_trust_anchor), MAP_WRITE));
    if (new_anchors == nullptr) {
        return false;
    }
    if (store.anchor_count != 0 && store.anchors != nullptr) {
        memcpy(new_anchors,
               store.anchors,
               store.anchor_count * sizeof(br_x509_trust_anchor));
    }
    store.anchors = new_anchors;
    store.anchor_capacity = new_capacity;
    return true;
}

bool parse_u16_range(const char* begin, const char* end, uint16_t& out) {
    if (begin == nullptr || end == nullptr || begin == end) {
        return false;
    }
    uint32_t value = 0;
    for (const char* p = begin; p != end; ++p) {
        if (!ascii_is_digit(*p)) {
            return false;
        }
        value = value * 10u + static_cast<uint32_t>(*p - '0');
        if (value > 65535u) {
            return false;
        }
    }
    out = static_cast<uint16_t>(value);
    return true;
}

int parse_decimal(const char* text) {
    if (text == nullptr || *text == '\0') {
        return -1;
    }
    int value = 0;
    while (*text != '\0') {
        if (!ascii_is_digit(*text)) {
            return -1;
        }
        value = value * 10 + static_cast<int>(*text - '0');
        ++text;
    }
    return value;
}

bool parse_decimal_range(const char* begin, const char* end, int& out) {
    if (begin == nullptr || end == nullptr || begin >= end) {
        return false;
    }
    int value = 0;
    for (const char* cursor = begin; cursor != end; ++cursor) {
        if (!ascii_is_digit(*cursor)) {
            return false;
        }
        value = value * 10 + static_cast<int>(*cursor - '0');
    }
    out = value;
    return true;
}

bool parse_hex_size(const char* text, size_t& out) {
    if (text == nullptr || *text == '\0') {
        return false;
    }
    size_t value = 0;
    while (*text != '\0' && *text != ';') {
        char ch = ascii_to_lower(*text);
        uint8_t digit = 0;
        if (ch >= '0' && ch <= '9') {
            digit = static_cast<uint8_t>(ch - '0');
        } else if (ch >= 'a' && ch <= 'f') {
            digit = static_cast<uint8_t>(10 + ch - 'a');
        } else {
            return false;
        }
        value = (value << 4) | digit;
        ++text;
    }
    out = value;
    return true;
}

bool parse_url(const char* text, Url& out) {
    if (text == nullptr || *text == '\0') {
        return false;
    }

    const char* cursor = text;
    if (ascii_starts_with_case_insensitive(cursor, "http://")) {
        out.scheme = Scheme::Http;
        out.port = 80;
        cursor += 7;
    } else if (ascii_starts_with_case_insensitive(cursor, "https://")) {
        out.scheme = Scheme::Https;
        out.port = 443;
        cursor += 8;
    } else {
        out.scheme = Scheme::Https;
        out.port = 443;
    }

    const char* host_begin = cursor;
    while (*cursor != '\0' && *cursor != '/' && *cursor != ':') {
        ++cursor;
    }
    if (cursor == host_begin) {
        return false;
    }
    copy_range(out.host, sizeof(out.host), host_begin, cursor);

    if (*cursor == ':') {
        ++cursor;
        const char* port_begin = cursor;
        while (*cursor != '\0' && *cursor != '/') {
            ++cursor;
        }
        if (!parse_u16_range(port_begin, cursor, out.port) || out.port == 0) {
            return false;
        }
    }

    if (*cursor == '\0') {
        strlcpy(out.path, "/", sizeof(out.path));
    } else {
        strlcpy(out.path, cursor, sizeof(out.path));
    }
    return true;
}

bool build_redirect_url(const Url& current, const char* location, char* out, size_t out_size) {
    if (location == nullptr || out == nullptr || out_size == 0 || *location == '\0') {
        return false;
    }
    if (ascii_starts_with_case_insensitive(location, "http://") ||
        ascii_starts_with_case_insensitive(location, "https://")) {
        strlcpy(out, location, out_size);
        return true;
    }
    if (location[0] == '/') {
        const char* scheme = current.scheme == Scheme::Https ? "https://" : "http://";
        size_t length = 0;
        size_t scheme_len = strlen(scheme);
        if (scheme_len >= out_size) {
            return false;
        }
        memcpy(out + length, scheme, scheme_len);
        length += scheme_len;
        size_t host_len = strlen(current.host);
        if (length + host_len >= out_size) {
            return false;
        }
        memcpy(out + length, current.host, host_len);
        length += host_len;
        bool need_port = (current.scheme == Scheme::Http && current.port != 80) ||
                         (current.scheme == Scheme::Https && current.port != 443);
        if (need_port) {
            char port_buf[8];
            size_t port_len = 0;
            uint16_t value = current.port;
            char rev[8];
            do {
                rev[port_len++] = static_cast<char>('0' + (value % 10));
                value = static_cast<uint16_t>(value / 10);
            } while (value != 0 && port_len < sizeof(rev));
            if (length + 1 + port_len >= out_size) {
                return false;
            }
            out[length++] = ':';
            for (size_t i = 0; i < port_len; ++i) {
                port_buf[i] = rev[port_len - 1 - i];
            }
            memcpy(out + length, port_buf, port_len);
            length += port_len;
        }
        size_t loc_len = strlen(location);
        if (length + loc_len >= out_size) {
            return false;
        }
        memcpy(out + length, location, loc_len);
        length += loc_len;
        out[length] = '\0';
        return true;
    }

    const char* slash = current.path + strlen(current.path);
    while (slash > current.path && slash[-1] != '/') {
        --slash;
    }
    char prefix[kMaxPath];
    copy_range(prefix, sizeof(prefix), current.path, slash);
    const char* scheme = current.scheme == Scheme::Https ? "https://" : "http://";
    size_t length = 0;
    size_t scheme_len = strlen(scheme);
    if (scheme_len >= out_size) {
        return false;
    }
    memcpy(out + length, scheme, scheme_len);
    length += scheme_len;
    size_t host_len = strlen(current.host);
    if (length + host_len >= out_size) {
        return false;
    }
    memcpy(out + length, current.host, host_len);
    length += host_len;
    bool need_port = (current.scheme == Scheme::Http && current.port != 80) ||
                     (current.scheme == Scheme::Https && current.port != 443);
    if (need_port) {
        char rev[8];
        size_t port_len = 0;
        uint16_t value = current.port;
        do {
            rev[port_len++] = static_cast<char>('0' + (value % 10));
            value = static_cast<uint16_t>(value / 10);
        } while (value != 0 && port_len < sizeof(rev));
        if (length + 1 + port_len >= out_size) {
            return false;
        }
        out[length++] = ':';
        for (size_t i = 0; i < port_len; ++i) {
            out[length++] = rev[port_len - 1 - i];
        }
    }
    if (length + strlen(prefix) + strlen(location) >= out_size) {
        return false;
    }
    memcpy(out + length, prefix, strlen(prefix));
    length += strlen(prefix);
    memcpy(out + length, location, strlen(location));
    length += strlen(location);
    out[length] = '\0';
    return true;
}

bool parse_ipv4_component(const char* begin, const char* end, uint8_t& out) {
    if (begin == end) {
        return false;
    }
    uint32_t value = 0;
    for (const char* p = begin; p != end; ++p) {
        if (!ascii_is_digit(*p)) {
            return false;
        }
        value = value * 10u + static_cast<uint32_t>(*p - '0');
        if (value > 255u) {
            return false;
        }
    }
    out = static_cast<uint8_t>(value);
    return true;
}

bool parse_ipv4_literal(const char* text, uint8_t out[4]) {
    if (text == nullptr || out == nullptr) {
        return false;
    }
    const char* begin = text;
    for (size_t i = 0; i < 4; ++i) {
        const char* cursor = begin;
        while (*cursor != '\0' && *cursor != '.') {
            ++cursor;
        }
        if (!parse_ipv4_component(begin, cursor, out[i])) {
            return false;
        }
        if (i == 3) {
            return *cursor == '\0';
        }
        if (*cursor != '.') {
            return false;
        }
        begin = cursor + 1;
    }
    return false;
}

bool open_tcpd_registry(uint32_t& handle, tcpd_protocol::Registry*& registry) {
    long shm = shared_memory_open(tcpd_protocol::kRegistryName,
                                  sizeof(tcpd_protocol::Registry));
    if (shm < 0) {
        print_line("browse: shared_memory_open(tcp.registry) failed");
        return false;
    }
    auto* info = static_cast<descriptor_defs::SharedMemoryInfo*>(
        map_anonymous(sizeof(descriptor_defs::SharedMemoryInfo), MAP_WRITE));
    if (info == nullptr) {
        descriptor_close(static_cast<uint32_t>(shm));
        return false;
    }
    long info_result = shared_memory_get_info(static_cast<uint32_t>(shm), info);
    uint64_t info_base = info->base;
    uint64_t info_length = info->length;
    unmap(info, sizeof(*info));
    if (info_result != 0 ||
        info_base == 0 ||
        info_length < sizeof(tcpd_protocol::Registry)) {
        print_line("browse: tcpd registry shm info invalid");
        descriptor_close(static_cast<uint32_t>(shm));
        return false;
    }
    handle = static_cast<uint32_t>(shm);
    registry = reinterpret_cast<tcpd_protocol::Registry*>(info_base);
    return true;
}

bool send_connect_request(uint32_t server_handle,
                          uint32_t reply_pipe_id,
                          const uint8_t ip[4],
                          uint16_t port) {
    tcpd_protocol::Message message{};
    tcpd_protocol::init_message(message, tcpd_protocol::kConnectRequest);
    message.connect_request.reply_pipe_id = reply_pipe_id;
    message.connect_request.remote_port = port;
    for (size_t i = 0; i < 4; ++i) {
        message.connect_request.remote_ip[i] = ip[i];
    }
    return tcpd_protocol::write_message(server_handle, message);
}

bool send_close_request(uint32_t server_handle, uint32_t connection_id) {
    tcpd_protocol::Message message{};
    tcpd_protocol::init_message(message, tcpd_protocol::kCloseRequest);
    message.close_request.connection_id = connection_id;
    return tcpd_protocol::write_message(server_handle, message);
}

bool send_payload(uint32_t server_handle,
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

bool tcp_connect(TcpConnection& conn, const uint8_t ip[4], uint16_t port) {
    uint32_t registry_handle = 0;
    tcpd_protocol::Registry* registry = nullptr;
    if (!open_tcpd_registry(registry_handle, registry)) {
        print_line("browse: failed to open tcpd registry");
        return false;
    }
    while (registry->magic != tcpd_protocol::kRegistryMagic ||
           registry->version != tcpd_protocol::kRegistryVersion ||
           registry->server_pipe_id == 0) {
        yield();
    }
    uint32_t server_pipe_id = registry->server_pipe_id;
    descriptor_close(registry_handle);

    uint64_t reply_flags = static_cast<uint64_t>(descriptor_defs::Flag::Readable) |
                           static_cast<uint64_t>(descriptor_defs::Flag::Async);
    long reply_pipe = pipe_open_new(reply_flags);
    if (reply_pipe < 0) {
        print_line("browse: failed to create reply pipe");
        return false;
    }
    descriptor_defs::PipeInfo reply_info{};
    if (pipe_get_info(static_cast<uint32_t>(reply_pipe), &reply_info) != 0 ||
        reply_info.id == 0) {
        print_line("browse: failed to query reply pipe");
        descriptor_close(static_cast<uint32_t>(reply_pipe));
        return false;
    }

    uint64_t server_flags = static_cast<uint64_t>(descriptor_defs::Flag::Writable) |
                            static_cast<uint64_t>(descriptor_defs::Flag::Async);
    long server_pipe = pipe_open_existing(server_flags, server_pipe_id);
    if (server_pipe < 0) {
        print_line("browse: failed to open tcpd server pipe");
        descriptor_close(static_cast<uint32_t>(reply_pipe));
        return false;
    }

    if (!send_connect_request(static_cast<uint32_t>(server_pipe),
                              reply_info.id,
                              ip,
                              port)) {
        print_line("browse: failed to send connect request");
        descriptor_close(static_cast<uint32_t>(reply_pipe));
        descriptor_close(static_cast<uint32_t>(server_pipe));
        return false;
    }

    uint32_t connect_waits = 0;
    for (;;) {
        tcpd_protocol::Message message{};
        if (!tcpd_protocol::read_message(static_cast<uint32_t>(reply_pipe), message)) {
            if (connect_waits++ >= kConnectWaitLimit) {
                print_line("browse: tcp connect timeout");
                descriptor_close(static_cast<uint32_t>(reply_pipe));
                descriptor_close(static_cast<uint32_t>(server_pipe));
                return false;
            }
            yield();
            continue;
        }
        if (message.type != tcpd_protocol::kConnectResponse) {
            continue;
        }
        if (message.connect_response.status != tcpd_protocol::kStatusOk) {
            print_line("browse: tcpd connect response not ok");
            descriptor_close(static_cast<uint32_t>(reply_pipe));
            descriptor_close(static_cast<uint32_t>(server_pipe));
            return false;
        }
        conn.server_pipe = static_cast<uint32_t>(server_pipe);
        conn.reply_pipe = static_cast<uint32_t>(reply_pipe);
        conn.connection_id = message.connect_response.connection_id;
        conn.closed = false;
        conn.pending_offset = 0;
        conn.pending_length = 0;
        return true;
    }
}

void tcp_close(TcpConnection& conn) {
    if (conn.server_pipe != 0 && conn.connection_id != 0) {
        (void)send_close_request(conn.server_pipe, conn.connection_id);
    }
    if (conn.reply_pipe != 0) {
        descriptor_close(conn.reply_pipe);
    }
    if (conn.server_pipe != 0) {
        descriptor_close(conn.server_pipe);
    }
    conn.server_pipe = 0;
    conn.reply_pipe = 0;
    conn.connection_id = 0;
    conn.closed = true;
    conn.pending_offset = 0;
    conn.pending_length = 0;
}

bool tcp_send_all(TcpConnection& conn, const uint8_t* data, size_t length) {
    size_t offset = 0;
    while (offset < length) {
        size_t chunk = length - offset;
        if (chunk > tcpd_protocol::kMaxPayload) {
            chunk = tcpd_protocol::kMaxPayload;
        }
        if (!send_payload(conn.server_pipe, conn.connection_id, data + offset, chunk)) {
            return false;
        }
        offset += chunk;
    }
    return true;
}

int tcp_read_some(TcpConnection& conn, uint8_t* out, size_t capacity) {
    if (capacity == 0) {
        return 0;
    }
    while (conn.pending_offset == conn.pending_length) {
        if (conn.closed) {
            return -1;
        }
        tcpd_protocol::Message message{};
        if (!tcpd_protocol::read_message(conn.reply_pipe, message)) {
            yield();
            continue;
        }
        if (message.type == tcpd_protocol::kDataEvent &&
            message.data_event.connection_id == conn.connection_id) {
            conn.pending_offset = 0;
            conn.pending_length = message.data_event.payload_length;
            memcpy(conn.pending, message.data_event.payload, conn.pending_length);
            break;
        }
        if (message.type == tcpd_protocol::kClosedEvent &&
            message.closed_event.connection_id == conn.connection_id) {
            conn.closed = true;
            return -1;
        }
    }

    size_t available = conn.pending_length - conn.pending_offset;
    if (available > capacity) {
        available = capacity;
    }
    memcpy(out, conn.pending + conn.pending_offset, available);
    conn.pending_offset += available;
    return static_cast<int>(available);
}

uint64_t read_tsc() {
    uint32_t lo = 0;
    uint32_t hi = 0;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

void seed_entropy(uint8_t out[32], const char* host, uint16_t port) {
    uint64_t mix = read_tsc();
    mix ^= reinterpret_cast<uintptr_t>(out);
    mix ^= reinterpret_cast<uintptr_t>(host);
    mix ^= static_cast<uint64_t>(port) << 16;
    for (size_t i = 0; i < 32; ++i) {
        yield();
        mix ^= read_tsc();
        mix ^= (mix << 13);
        mix ^= (mix >> 7);
        mix ^= (mix << 17);
        out[i] = static_cast<uint8_t>((mix >> ((i % 8) * 8)) & 0xFFu);
    }
}

void dn_append(void* ctx, const void* src, size_t len) {
    auto* buffer = static_cast<Buffer*>(ctx);
    (void)buffer_append(*buffer, src, len);
}

struct PemDecodeContext {
    Buffer der;
    bool collecting;
    bool error;
};

void pem_dest(void* ctx, const void* src, size_t len) {
    auto* pem = static_cast<PemDecodeContext*>(ctx);
    if (!buffer_append(pem->der, src, len)) {
        pem->error = true;
    }
}

bool append_trust_anchor_from_der(TrustStore& store, const uint8_t* der, size_t der_len) {
    print_line("browse: decode trust anchor");
    if (der == nullptr || der_len == 0) {
        print_line("browse: der empty");
        return false;
    }

    Buffer dn{};
    br_x509_decoder_context decoder{};
    br_x509_decoder_init(&decoder, dn_append, &dn);
    br_x509_decoder_push(&decoder, der, der_len);
    if (br_x509_decoder_last_error(&decoder) != 0) {
        print_line("browse: x509 decode failed");
        return false;
    }

    br_x509_pkey* key = br_x509_decoder_get_pkey(&decoder);
    if (key == nullptr || dn.size == 0) {
        print_line("browse: x509 missing key or dn");
        return false;
    }

    if (!trust_store_reserve(store, store.anchor_count + 1)) {
        print_line("browse: trust_store_reserve failed");
        return false;
    }

    br_x509_trust_anchor anchor{};
    anchor.flags = br_x509_decoder_isCA(&decoder) ? BR_X509_TA_CA : 0;

    auto* dn_copy = static_cast<unsigned char*>(alloc_persistent_bytes(dn.size));
    if (dn_copy == nullptr) {
        print_line("browse: dn alloc failed");
        return false;
    }
    memcpy(dn_copy, dn.data, dn.size);
    anchor.dn.data = dn_copy;
    anchor.dn.len = dn.size;
    anchor.pkey.key_type = key->key_type;

    if (key->key_type == BR_KEYTYPE_RSA) {
        auto* n_copy = static_cast<unsigned char*>(alloc_persistent_bytes(key->key.rsa.nlen));
        auto* e_copy = static_cast<unsigned char*>(alloc_persistent_bytes(key->key.rsa.elen));
        if (n_copy == nullptr || e_copy == nullptr) {
            print_line("browse: rsa alloc failed");
            return false;
        }
        memcpy(n_copy, key->key.rsa.n, key->key.rsa.nlen);
        memcpy(e_copy, key->key.rsa.e, key->key.rsa.elen);
        anchor.pkey.key.rsa.n = n_copy;
        anchor.pkey.key.rsa.nlen = key->key.rsa.nlen;
        anchor.pkey.key.rsa.e = e_copy;
        anchor.pkey.key.rsa.elen = key->key.rsa.elen;
    } else if (key->key_type == BR_KEYTYPE_EC) {
        auto* q_copy = static_cast<unsigned char*>(alloc_persistent_bytes(key->key.ec.qlen));
        if (q_copy == nullptr) {
            print_line("browse: ec alloc failed");
            return false;
        }
        memcpy(q_copy, key->key.ec.q, key->key.ec.qlen);
        anchor.pkey.key.ec.curve = key->key.ec.curve;
        anchor.pkey.key.ec.q = q_copy;
        anchor.pkey.key.ec.qlen = key->key.ec.qlen;
    } else {
        print_line("browse: unsupported key type");
        return false;
    }

    store.anchors[store.anchor_count++] = anchor;
    print_line("browse: trust anchor appended");
    return true;
}

bool load_trust_store(TrustStore& store) {
    print_line("browse: load_trust_store begin");
    Buffer pem_bytes{};
    if (!load_file(".../config/ssl/cacert.pem", pem_bytes) &&
        !load_file("config/ssl/cacert.pem", pem_bytes)) {
        print_line("browse: no cacert.pem");
        return false;
    }
    print_line("browse: pem file loaded");

    br_pem_decoder_context pem{};
    br_pem_decoder_init(&pem);
    PemDecodeContext pem_ctx{};
    print_line("browse: pem decoder init");

    size_t offset = 0;
    while (offset < pem_bytes.size) {
        size_t consumed = br_pem_decoder_push(&pem,
                                              pem_bytes.data + offset,
                                              pem_bytes.size - offset);
        offset += consumed;

        int event = br_pem_decoder_event(&pem);
        if (event == 0) {
            continue;
        }
        if (event == BR_PEM_BEGIN_OBJ) {
            print_line("browse: pem begin");
            buffer_clear(pem_ctx.der);
            pem_ctx.collecting = true;
            pem_ctx.error = false;
            br_pem_decoder_setdest(&pem,
                                   pem_ctx.collecting ? pem_dest : nullptr,
                                   &pem_ctx);
            continue;
        }
        if (event == BR_PEM_END_OBJ) {
            print_line("browse: pem end");
            if (pem_ctx.collecting && !pem_ctx.error && pem_ctx.der.size != 0) {
                (void)append_trust_anchor_from_der(store, pem_ctx.der.data, pem_ctx.der.size);
            }
            pem_ctx.collecting = false;
            pem_ctx.error = false;
            buffer_clear(pem_ctx.der);
            br_pem_decoder_setdest(&pem, nullptr, nullptr);
            continue;
        }
        if (event == BR_PEM_ERROR) {
            print_line("browse: pem error");
            pem_ctx.collecting = false;
            pem_ctx.error = true;
            buffer_clear(pem_ctx.der);
            br_pem_decoder_setdest(&pem, nullptr, nullptr);
        }
    }
    print_line("browse: load_trust_store done");
    return store.anchor_count != 0;
}

bool ensure_trust_store_loaded() {
    print_line("browse: ensure_trust_store_loaded");
    if (g_trust_store == reinterpret_cast<TrustStore*>(1)) {
        print_line("browse: allocating trust store state");
        g_trust_store = static_cast<TrustStore*>(map_anonymous(sizeof(TrustStore), MAP_WRITE));
        if (g_trust_store == nullptr) {
            print_line("browse: trust store state alloc failed");
            return false;
        }
        for (size_t i = 0; i < sizeof(*g_trust_store); ++i) {
            reinterpret_cast<uint8_t*>(g_trust_store)[i] = 0;
        }
    }
    if (g_trust_store->attempted) {
        print_line("browse: trust store already attempted");
        return g_trust_store->loaded;
    }
    g_trust_store->attempted = true;
    print_line("browse: trust store loading");
    g_trust_store->loaded = load_trust_store(*g_trust_store);
    if (g_trust_store->loaded) {
        print_line("browse: trust store loaded");
    } else {
        print_line("browse: trust store unavailable");
    }
    return g_trust_store->loaded;
}

int ignore_certificate_time(void*,
                            uint32_t,
                            uint32_t,
                            uint32_t,
                            uint32_t) {
    return 0;
}

void insecure_start_chain(const br_x509_class** ctx, const char*) {
    auto* xc = reinterpret_cast<InsecureX509Context*>(const_cast<br_x509_class**>(ctx));
    xc->vtable = &xc->vtable[0];
    xc->cert_count = 0;
    xc->end_error = 0;
    xc->usages = BR_KEYTYPE_KEYX | BR_KEYTYPE_SIGN;
    xc->have_pkey = false;
}

void insecure_start_cert(const br_x509_class** ctx, uint32_t) {
    auto* xc = reinterpret_cast<InsecureX509Context*>(const_cast<br_x509_class**>(ctx));
    if (xc->cert_count == 0) {
        br_x509_decoder_init(&xc->decoder, nullptr, nullptr);
    }
}

void insecure_append(const br_x509_class** ctx, const unsigned char* buf, size_t len) {
    auto* xc = reinterpret_cast<InsecureX509Context*>(const_cast<br_x509_class**>(ctx));
    if (xc->cert_count == 0) {
        br_x509_decoder_push(&xc->decoder, buf, len);
    }
}

void insecure_end_cert(const br_x509_class** ctx) {
    auto* xc = reinterpret_cast<InsecureX509Context*>(const_cast<br_x509_class**>(ctx));
    if (xc->cert_count == 0) {
        int err = br_x509_decoder_last_error(&xc->decoder);
        if (err == 0 && br_x509_decoder_get_pkey(&xc->decoder) != nullptr) {
            xc->have_pkey = true;
        } else if (xc->end_error == 0) {
            xc->end_error = static_cast<unsigned>(err);
        }
    }
    ++xc->cert_count;
}

unsigned insecure_end_chain(const br_x509_class** ctx) {
    auto* xc = reinterpret_cast<InsecureX509Context*>(const_cast<br_x509_class**>(ctx));
    if (!xc->have_pkey) {
        return xc->end_error != 0 ? xc->end_error : static_cast<unsigned>(BR_ERR_X509_EMPTY_CHAIN);
    }
    return 0;
}

const br_x509_pkey* insecure_get_pkey(const br_x509_class* const* ctx, unsigned* usages) {
    auto* xc = reinterpret_cast<const InsecureX509Context*>(ctx);
    if (usages != nullptr) {
        *usages = xc->usages;
    }
    return br_x509_decoder_get_pkey(const_cast<br_x509_decoder_context*>(&xc->decoder));
}

const br_x509_class kInsecureX509Vtable = {
    sizeof(InsecureX509Context),
    insecure_start_chain,
    insecure_start_cert,
    insecure_append,
    insecure_end_cert,
    insecure_end_chain,
    insecure_get_pkey,
};

void init_tls_client(Stream& stream, const char* host) {
    print_line("browse: init_tls_client");
    stream.tls_mode = TlsMode::Insecure;
    if (ensure_trust_store_loaded() && g_trust_store->anchor_count != 0) {
        print_line("browse: using verified tls mode");
        br_ssl_client_init_full(&stream.ssl_client,
                                &stream.x509_minimal,
                                g_trust_store->anchors,
                                g_trust_store->anchor_count);
        print_line("browse: br_ssl_client_init_full ok");
        br_x509_minimal_set_time_callback(&stream.x509_minimal,
                                          nullptr,
                                          ignore_certificate_time);
        print_line("browse: time callback installed");
        stream.tls_mode = TlsMode::VerifiedNoTime;
    } else {
        print_line("browse: using insecure tls mode");
        stream.insecure_x509.vtable = &kInsecureX509Vtable;
        stream.insecure_x509.cert_count = 0;
        stream.insecure_x509.end_error = 0;
        stream.insecure_x509.usages = BR_KEYTYPE_KEYX | BR_KEYTYPE_SIGN;
        stream.insecure_x509.have_pkey = false;

        br_ssl_client_zero(&stream.ssl_client);
        br_ssl_engine_set_versions(&stream.ssl_client.eng, BR_TLS10, BR_TLS12);
        br_ssl_client_set_default_rsapub(&stream.ssl_client);
        br_ssl_engine_set_default_rsavrfy(&stream.ssl_client.eng);
        br_ssl_engine_set_default_ecdsa(&stream.ssl_client.eng);

        static const uint16_t suites[] = {
            BR_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,
            BR_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
            BR_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
            BR_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
            BR_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
            BR_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
            BR_TLS_RSA_WITH_AES_128_GCM_SHA256,
            BR_TLS_RSA_WITH_AES_256_GCM_SHA384,
            BR_TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA256,
            BR_TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA256,
            BR_TLS_ECDHE_ECDSA_WITH_AES_256_CBC_SHA384,
            BR_TLS_ECDHE_RSA_WITH_AES_256_CBC_SHA384,
            BR_TLS_RSA_WITH_AES_128_CBC_SHA256,
            BR_TLS_RSA_WITH_AES_256_CBC_SHA256,
            BR_TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA,
            BR_TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA,
            BR_TLS_ECDHE_ECDSA_WITH_AES_256_CBC_SHA,
            BR_TLS_ECDHE_RSA_WITH_AES_256_CBC_SHA,
            BR_TLS_RSA_WITH_AES_128_CBC_SHA,
            BR_TLS_RSA_WITH_AES_256_CBC_SHA,
        };
        br_ssl_engine_set_suites(&stream.ssl_client.eng, suites, sizeof(suites) / sizeof(suites[0]));

        static const br_hash_class* hashes[] = {
            &br_md5_vtable,
            &br_sha1_vtable,
            &br_sha224_vtable,
            &br_sha256_vtable,
            &br_sha384_vtable,
            &br_sha512_vtable,
        };
        for (int id = br_md5_ID; id <= br_sha512_ID; ++id) {
            br_ssl_engine_set_hash(&stream.ssl_client.eng, id, hashes[id - 1]);
        }

        br_ssl_engine_set_x509(&stream.ssl_client.eng,
                               reinterpret_cast<const br_x509_class**>(&stream.insecure_x509.vtable));
        br_ssl_engine_set_prf10(&stream.ssl_client.eng, &br_tls10_prf);
        br_ssl_engine_set_prf_sha256(&stream.ssl_client.eng, &br_tls12_sha256_prf);
        br_ssl_engine_set_prf_sha384(&stream.ssl_client.eng, &br_tls12_sha384_prf);
        br_ssl_engine_set_default_aes_cbc(&stream.ssl_client.eng);
        br_ssl_engine_set_default_aes_ccm(&stream.ssl_client.eng);
        br_ssl_engine_set_default_aes_gcm(&stream.ssl_client.eng);
        br_ssl_engine_set_default_des_cbc(&stream.ssl_client.eng);
        br_ssl_engine_set_default_chapol(&stream.ssl_client.eng);
        print_line("browse: insecure tls engine configured");
    }
    br_ssl_engine_set_buffer(&stream.ssl_client.eng,
                             stream.ssl_iobuf,
                             sizeof(stream.ssl_iobuf),
                             1);
    print_line("browse: tls buffer configured");
    uint8_t entropy[32];
    seed_entropy(entropy, host, 443);
    print_line("browse: entropy seeded");
    br_ssl_engine_inject_entropy(&stream.ssl_client.eng, entropy, sizeof(entropy));
    print_line("browse: entropy injected");
    br_ssl_client_reset(&stream.ssl_client, host, 0);
    print_line("browse: tls client reset");
}

int tcp_low_read(void* context, unsigned char* data, size_t len) {
    auto* tcp = static_cast<TcpConnection*>(context);
    return tcp_read_some(*tcp, data, len);
}

int tcp_low_write(void* context, const unsigned char* data, size_t len) {
    auto* tcp = static_cast<TcpConnection*>(context);
    if (tcp_send_all(*tcp, data, len)) {
        return static_cast<int>(len);
    }
    return -1;
}

bool stream_connect(Stream& stream, const Url& url) {
    uint8_t ip[4];
    bool host_is_ip = parse_ipv4_literal(url.host, ip);
    if (!host_is_ip) {
        print("browse: resolving ");
        print_line(url.host);
        if (!usernet::dns::resolve_a(url.host, ip)) {
            print_line("browse: dns lookup failed");
            return false;
        }
    }
    print("browse: connecting to ");
    print(url.host);
    print(":");
    char port_buf[8];
    size_t port_len = 0;
    uint16_t value = url.port;
    char rev[8];
    do {
        rev[port_len++] = static_cast<char>('0' + (value % 10));
        value = static_cast<uint16_t>(value / 10);
    } while (value != 0 && port_len < sizeof(rev));
    for (size_t i = 0; i < port_len; ++i) {
        port_buf[i] = rev[port_len - 1 - i];
    }
    print_raw(port_buf, port_len);
    print("\n");
    if (!tcp_connect(*stream.tcp, ip, url.port)) {
        return false;
    }
    if (!stream.tls) {
        return true;
    }
    init_tls_client(stream, url.host);
    br_sslio_init(&stream.ssl_io,
                  &stream.ssl_client.eng,
                  tcp_low_read,
                  stream.tcp,
                  tcp_low_write,
                  stream.tcp);
    return true;
}

void stream_close(Stream& stream) {
    tcp_close(*stream.tcp);
}

int stream_read(Stream& stream, uint8_t* data, size_t len) {
    if (!stream.tls) {
        return tcp_read_some(*stream.tcp, data, len);
    }
    return br_sslio_read(&stream.ssl_io, data, len);
}

bool stream_write_all(Stream& stream, const uint8_t* data, size_t len) {
    if (!stream.tls) {
        return tcp_send_all(*stream.tcp, data, len);
    }
    return br_sslio_write_all(&stream.ssl_io, data, len) == 0 &&
           br_sslio_flush(&stream.ssl_io) == 0;
}

bool stream_read_line(Stream& stream, char* out, size_t out_size) {
    if (out == nullptr || out_size == 0) {
        return false;
    }
    size_t length = 0;
    while (length + 1 < out_size) {
        uint8_t ch = 0;
        int got = stream_read(stream, &ch, 1);
        if (got <= 0) {
            return false;
        }
        if (ch == '\r') {
            continue;
        }
        if (ch == '\n') {
            out[length] = '\0';
            return true;
        }
        out[length++] = static_cast<char>(ch);
    }
    out[out_size - 1] = '\0';
    return true;
}

bool append_request(char* request,
                    size_t capacity,
                    size_t& length,
                    const char* text) {
    size_t text_len = strlen(text);
    if (length + text_len > capacity) {
        return false;
    }
    memcpy(request + length, text, text_len);
    length += text_len;
    return true;
}

bool send_http_request(Stream& stream, const Url& url) {
    char request[1536];
    size_t length = 0;
    if (!append_request(request, sizeof(request), length, "GET ") ||
        !append_request(request, sizeof(request), length, url.path) ||
        !append_request(request, sizeof(request), length, " HTTP/1.1\r\nHost: ") ||
        !append_request(request, sizeof(request), length, url.host)) {
        return false;
    }
    bool need_port = (url.scheme == Scheme::Http && url.port != 80) ||
                     (url.scheme == Scheme::Https && url.port != 443);
    if (need_port) {
        size_t port_len = 0;
        uint16_t value = url.port;
        char rev[8];
        do {
            rev[port_len++] = static_cast<char>('0' + (value % 10));
            value = static_cast<uint16_t>(value / 10);
        } while (value != 0 && port_len < sizeof(rev));
        if (length + 1 + port_len > sizeof(request)) {
            return false;
        }
        request[length++] = ':';
        for (size_t i = 0; i < port_len; ++i) {
            request[length++] = rev[port_len - 1 - i];
        }
    }
    if (!append_request(request, sizeof(request), length,
                        "\r\nUser-Agent: neutrino-browse/0.1\r\n"
                        "Accept: text/html, text/plain, */*\r\n"
                        "Accept-Encoding: identity\r\n"
                        "Connection: close\r\n\r\n")) {
        return false;
    }
    return stream_write_all(stream,
                            reinterpret_cast<const uint8_t*>(request),
                            length);
}

void trim_spaces(char* text) {
    if (text == nullptr) {
        return;
    }
    size_t start = 0;
    while (text[start] == ' ' || text[start] == '\t') {
        ++start;
    }
    size_t length = strlen(text + start);
    memmove(text, text + start, length + 1);
    while (length != 0 &&
           (text[length - 1] == ' ' || text[length - 1] == '\t')) {
        text[--length] = '\0';
    }
}

void init_response_meta(HttpResponseMeta& meta) {
    meta.status_code = 0;
    meta.chunked = false;
    meta.have_content_length = false;
    meta.content_length = 0;
    meta.is_html = false;
    meta.is_text = false;
    meta.location[0] = '\0';
}

bool read_response_headers(Stream& stream, HttpResponseMeta& meta) {
    init_response_meta(meta);
    char line[1024];
    if (!stream_read_line(stream, line, sizeof(line))) {
        return false;
    }
    int first_space = ascii_find_char(line, ' ');
    if (first_space >= 0) {
        const char* status_begin = line + first_space + 1;
        const char* status_end = status_begin;
        while (*status_end != '\0' && *status_end != ' ') {
            ++status_end;
        }
        int status_code = 0;
        if (parse_decimal_range(status_begin, status_end, status_code)) {
            meta.status_code = status_code;
        }
    }
    for (;;) {
        if (!stream_read_line(stream, line, sizeof(line))) {
            return false;
        }
        if (line[0] == '\0') {
            return true;
        }
        int colon = ascii_find_char(line, ':');
        if (colon <= 0) {
            continue;
        }
        line[colon] = '\0';
        char* value = line + colon + 1;
        trim_spaces(value);
        if (ascii_starts_with_case_insensitive(line, "content-type")) {
            if (ascii_starts_with_case_insensitive(value, "text/html") ||
                ascii_starts_with_case_insensitive(value, "application/xhtml+xml")) {
                meta.is_html = true;
                meta.is_text = true;
            } else if (ascii_starts_with_case_insensitive(value, "text/") ||
                       ascii_starts_with_case_insensitive(value, "application/json") ||
                       ascii_starts_with_case_insensitive(value, "application/xml")) {
                meta.is_text = true;
            }
        } else if (ascii_starts_with_case_insensitive(line, "content-length")) {
            int value_num = parse_decimal(value);
            if (value_num >= 0) {
                meta.have_content_length = true;
                meta.content_length = static_cast<size_t>(value_num);
            }
        } else if (ascii_starts_with_case_insensitive(line, "transfer-encoding")) {
            if (ascii_starts_with_case_insensitive(value, "chunked")) {
                meta.chunked = true;
            }
        } else if (ascii_starts_with_case_insensitive(line, "location")) {
            strlcpy(meta.location, value, sizeof(meta.location));
        }
    }
}

void renderer_output_char(HtmlRenderer& renderer, char ch) {
    if (ch == '\r') {
        return;
    }
    if (ch == '\n') {
        print("\n");
        renderer.column = 0;
        renderer.pending_space = false;
        renderer.last_was_newline = true;
        return;
    }
    if (renderer.pending_space && renderer.column != 0) {
        if (renderer.column + 1 >= kRenderWidth) {
            print("\n");
            renderer.column = 0;
        } else {
            print(" ");
            ++renderer.column;
        }
    }
    renderer.pending_space = false;
    if (renderer.column >= kRenderWidth && ch != ' ') {
        print("\n");
        renderer.column = 0;
    }
    print_raw(&ch, 1);
    ++renderer.column;
    renderer.last_was_newline = false;
    renderer.have_visible_text = true;
}

void renderer_emit_break(HtmlRenderer& renderer) {
    if (!renderer.last_was_newline) {
        print("\n");
        renderer.column = 0;
        renderer.pending_space = false;
        renderer.last_was_newline = true;
    }
}

void renderer_emit_paragraph(HtmlRenderer& renderer) {
    if (!renderer.last_was_newline) {
        print("\n");
    }
    print("\n");
    renderer.column = 0;
    renderer.pending_space = false;
    renderer.last_was_newline = true;
}

void renderer_finish_tag(HtmlRenderer& renderer) {
    renderer.tag_buffer[renderer.tag_length] = '\0';
    char* tag = renderer.tag_buffer;
    size_t start = 0;
    while (tag[start] == ' ' || tag[start] == '\t') {
        ++start;
    }
    bool closing = false;
    if (tag[start] == '/') {
        closing = true;
        ++start;
    }
    size_t name_len = 0;
    while (tag[start + name_len] != '\0' &&
           tag[start + name_len] != ' ' &&
           tag[start + name_len] != '\t' &&
           tag[start + name_len] != '/') {
        tag[name_len] = ascii_to_lower(tag[start + name_len]);
        ++name_len;
    }
    tag[name_len] = '\0';

    if (renderer.in_comment) {
        if (name_len >= 2 &&
            tag[name_len - 1] == '-' &&
            tag[name_len - 2] == '-') {
            renderer.in_comment = false;
        }
        return;
    }
    if (!closing && strcmp(tag, "!--") == 0) {
        renderer.in_comment = true;
        return;
    }
    if (!closing && strcmp(tag, "script") == 0) {
        renderer.in_script = true;
        return;
    }
    if (!closing && strcmp(tag, "style") == 0) {
        renderer.in_style = true;
        return;
    }
    if (closing && strcmp(tag, "script") == 0) {
        renderer.in_script = false;
        return;
    }
    if (closing && strcmp(tag, "style") == 0) {
        renderer.in_style = false;
        return;
    }
    if (renderer.in_script || renderer.in_style) {
        return;
    }

    bool paragraph = strcmp(tag, "p") == 0 ||
                     strcmp(tag, "div") == 0 ||
                     strcmp(tag, "section") == 0 ||
                     strcmp(tag, "article") == 0 ||
                     strcmp(tag, "header") == 0 ||
                     strcmp(tag, "footer") == 0 ||
                     strcmp(tag, "h1") == 0 ||
                     strcmp(tag, "h2") == 0 ||
                     strcmp(tag, "h3") == 0 ||
                     strcmp(tag, "h4") == 0 ||
                     strcmp(tag, "h5") == 0 ||
                     strcmp(tag, "h6") == 0;
    bool line_break = strcmp(tag, "br") == 0 ||
                      strcmp(tag, "li") == 0 ||
                      strcmp(tag, "tr") == 0;
    if (paragraph) {
        renderer_emit_paragraph(renderer);
    } else if (line_break) {
        renderer_emit_break(renderer);
        if (!closing && strcmp(tag, "li") == 0) {
            print("* ");
            renderer.column = 2;
            renderer.last_was_newline = false;
        }
    }
}

void renderer_emit_entity(HtmlRenderer& renderer) {
    renderer.entity_buffer[renderer.entity_length] = '\0';
    const char* decoded = nullptr;
    if (strcmp(renderer.entity_buffer, "amp") == 0) {
        decoded = "&";
    } else if (strcmp(renderer.entity_buffer, "lt") == 0) {
        decoded = "<";
    } else if (strcmp(renderer.entity_buffer, "gt") == 0) {
        decoded = ">";
    } else if (strcmp(renderer.entity_buffer, "quot") == 0) {
        decoded = "\"";
    } else if (strcmp(renderer.entity_buffer, "nbsp") == 0) {
        decoded = " ";
    }
    if (decoded != nullptr) {
        for (size_t i = 0; decoded[i] != '\0'; ++i) {
            renderer_output_char(renderer, decoded[i]);
        }
    }
}

void renderer_init(HtmlRenderer& renderer) {
    renderer.in_tag = false;
    renderer.in_entity = false;
    renderer.in_comment = false;
    renderer.in_script = false;
    renderer.in_style = false;
    renderer.pending_space = false;
    renderer.last_was_newline = true;
    renderer.have_visible_text = false;
    renderer.tag_length = 0;
    renderer.entity_length = 0;
    renderer.column = 0;
}

void renderer_process_html(HtmlRenderer& renderer, const uint8_t* data, size_t length) {
    for (size_t i = 0; i < length; ++i) {
        char ch = static_cast<char>(data[i]);
        if (renderer.in_comment) {
            if (!renderer.in_tag) {
                if (ch == '<') {
                    renderer.in_tag = true;
                    renderer.tag_length = 0;
                }
                continue;
            }
            if (ch == '>') {
                renderer.in_tag = false;
                renderer_finish_tag(renderer);
            } else if (renderer.tag_length + 1 < sizeof(renderer.tag_buffer)) {
                renderer.tag_buffer[renderer.tag_length++] = ch;
            }
            continue;
        }
        if (renderer.in_tag) {
            if (ch == '>') {
                renderer.in_tag = false;
                renderer_finish_tag(renderer);
                renderer.tag_length = 0;
            } else if (renderer.tag_length + 1 < sizeof(renderer.tag_buffer)) {
                renderer.tag_buffer[renderer.tag_length++] = ch;
            }
            continue;
        }
        if (renderer.in_script || renderer.in_style) {
            if (ch == '<') {
                renderer.in_tag = true;
                renderer.tag_length = 0;
            }
            continue;
        }
        if (renderer.in_entity) {
            if (ch == ';') {
                renderer_emit_entity(renderer);
                renderer.in_entity = false;
                renderer.entity_length = 0;
            } else if (renderer.entity_length + 1 < sizeof(renderer.entity_buffer)) {
                renderer.entity_buffer[renderer.entity_length++] = ascii_to_lower(ch);
            } else {
                renderer.in_entity = false;
                renderer.entity_length = 0;
            }
            continue;
        }
        if (ch == '<') {
            renderer.in_tag = true;
            renderer.tag_length = 0;
            continue;
        }
        if (ch == '&') {
            renderer.in_entity = true;
            renderer.entity_length = 0;
            continue;
        }
        if (ch == '\t' || ch == '\n' || ch == '\r' || ch == ' ') {
            renderer.pending_space = true;
            continue;
        }
        renderer_output_char(renderer, ch);
    }
}

void renderer_process_text(HtmlRenderer& renderer, const uint8_t* data, size_t length) {
    for (size_t i = 0; i < length; ++i) {
        char ch = static_cast<char>(data[i]);
        if (ch == '\r') {
            continue;
        }
        if (ch == '\n') {
            renderer_emit_break(renderer);
            continue;
        }
        if (ch == '\t' || ch == ' ') {
            renderer.pending_space = true;
            continue;
        }
        if (static_cast<unsigned char>(ch) < 0x20 && ch != '\n') {
            continue;
        }
        renderer_output_char(renderer, ch);
    }
}

bool read_chunk_data(Stream& stream,
                     HtmlRenderer& renderer,
                     size_t size,
                     bool html_mode) {
    uint8_t buffer[kIoBufferSize];
    size_t remaining = size;
    while (remaining != 0) {
        size_t want = remaining;
        if (want > sizeof(buffer)) {
            want = sizeof(buffer);
        }
        int got = stream_read(stream, buffer, want);
        if (got <= 0) {
            return false;
        }
        if (html_mode) {
            renderer_process_html(renderer, buffer, static_cast<size_t>(got));
        } else {
            renderer_process_text(renderer, buffer, static_cast<size_t>(got));
        }
        remaining -= static_cast<size_t>(got);
    }
    uint8_t crlf[2];
    return stream_read(stream, crlf, 2) == 2;
}

bool render_body(Stream& stream, const HttpResponseMeta& meta) {
    HtmlRenderer renderer;
    renderer_init(renderer);

    if (!meta.is_text) {
        print_line("[non-text response omitted]");
        return true;
    }

    if (meta.chunked) {
        char line[128];
        for (;;) {
            if (!stream_read_line(stream, line, sizeof(line))) {
                return false;
            }
            size_t chunk_size = 0;
            if (!parse_hex_size(line, chunk_size)) {
                return false;
            }
            if (chunk_size == 0) {
                while (stream_read_line(stream, line, sizeof(line)) && line[0] != '\0') {
                }
                break;
            }
            if (!read_chunk_data(stream, renderer, chunk_size, meta.is_html)) {
                return false;
            }
        }
    } else if (meta.have_content_length) {
        uint8_t buffer[kIoBufferSize];
        size_t remaining = meta.content_length;
        while (remaining != 0) {
            size_t want = remaining;
            if (want > sizeof(buffer)) {
                want = sizeof(buffer);
            }
            int got = stream_read(stream, buffer, want);
            if (got <= 0) {
                return false;
            }
            if (meta.is_html) {
                renderer_process_html(renderer, buffer, static_cast<size_t>(got));
            } else {
                renderer_process_text(renderer, buffer, static_cast<size_t>(got));
            }
            remaining -= static_cast<size_t>(got);
        }
    } else {
        uint8_t buffer[kIoBufferSize];
        while (true) {
            int got = stream_read(stream, buffer, sizeof(buffer));
            if (got <= 0) {
                break;
            }
            if (meta.is_html) {
                renderer_process_html(renderer, buffer, static_cast<size_t>(got));
            } else {
                renderer_process_text(renderer, buffer, static_cast<size_t>(got));
            }
        }
    }

    if (!renderer.last_was_newline) {
        print("\n");
    }
    if (!renderer.have_visible_text) {
        print_line("[empty]");
    }
    return true;
}

char read_char_blocking(uint32_t keyboard) {
    while (true) {
        descriptor_defs::KeyboardEvent events[8]{};
        long r = descriptor_read(keyboard, events, sizeof(events));
        if (r <= 0) {
            yield();
            continue;
        }
        size_t count = static_cast<size_t>(r) / sizeof(events[0]);
        for (size_t i = 0; i < count; ++i) {
            if (!keyboard::is_pressed(events[i]) || keyboard::is_extended(events[i])) {
                continue;
            }
            char ch = keyboard::scancode_to_char(events[i].scancode, events[i].mods);
            if (ch != 0) {
                return ch;
            }
        }
    }
}

size_t prompt_line(char* out, size_t out_capacity) {
    if (out == nullptr || out_capacity == 0) {
        return 0;
    }
    long keyboard = descriptor_open(kDescKeyboard, 0);
    if (keyboard < 0) {
        return 0;
    }
    print("url: ");
    size_t length = 0;
    while (true) {
        char ch = read_char_blocking(static_cast<uint32_t>(keyboard));
        if (ch == '\n' || ch == '\r') {
            print("\n");
            break;
        }
        if (ch == '\b' || ch == 0x7F) {
            if (length != 0) {
                --length;
                out[length] = '\0';
                print("\b \b");
            }
            continue;
        }
        if (ch < 0x20 || ch > 0x7E) {
            continue;
        }
        if (length + 1 < out_capacity) {
            out[length++] = ch;
            out[length] = '\0';
            print_raw(&ch, 1);
        }
    }
    descriptor_close(static_cast<uint32_t>(keyboard));
    return length;
}

bool is_redirect_status(int status_code) {
    return status_code == 301 || status_code == 302 || status_code == 303 ||
           status_code == 307 || status_code == 308;
}

int browse_url(const char* initial_url) {
    char current_url[kMaxUrl];
    strlcpy(current_url, initial_url, sizeof(current_url));

    auto* stream = static_cast<Stream*>(map_anonymous(sizeof(Stream), MAP_WRITE));
    if (stream == nullptr) {
        print_line("browse: failed to allocate TLS state");
        return 1;
    }

    for (uint32_t redirect = 0; redirect <= kMaxRedirects; ++redirect) {
        Url url{};
        if (!parse_url(current_url, url)) {
            print_line("browse: invalid URL");
            return 1;
        }

        TcpConnection tcp{};
        for (size_t i = 0; i < sizeof(*stream); ++i) {
            reinterpret_cast<uint8_t*>(stream)[i] = 0;
        }
        stream->tcp = &tcp;
        stream->tls = (url.scheme == Scheme::Https);
        if (!stream_connect(*stream, url)) {
            print_line("browse: connect failed");
            return 1;
        }
        if (!send_http_request(*stream, url)) {
            stream_close(*stream);
            print_line("browse: request failed");
            return 1;
        }

        HttpResponseMeta meta{};
        if (!read_response_headers(*stream, meta)) {
            stream_close(*stream);
            print_line("browse: failed to read response");
            return 1;
        }

        if (stream->tls_mode == TlsMode::Insecure) {
            print_line("[https: certificate validation disabled; no CA bundle loaded]");
        } else if (stream->tls_mode == TlsMode::VerifiedNoTime) {
            print_line("[https: CA bundle loaded; certificate dates not checked]");
        }

        if (is_redirect_status(meta.status_code) && meta.location[0] != '\0') {
            char next_url[kMaxUrl];
            if (!build_redirect_url(url, meta.location, next_url, sizeof(next_url))) {
                stream_close(*stream);
                print_line("browse: redirect URL too long");
                return 1;
            }
            stream_close(*stream);
            print("redirect -> ");
            print_line(next_url);
            strlcpy(current_url, next_url, sizeof(current_url));
            continue;
        }

        if (!render_body(*stream, meta)) {
            stream_close(*stream);
            print_line("browse: failed to read body");
            return 1;
        }
        stream_close(*stream);
        return 0;
    }

    print_line("browse: too many redirects");
    return 1;
}

}  // namespace

int main(uint64_t arg_ptr, uint64_t) {
    g_console = descriptor_open(kDescConsole, 0);
    if (g_console < 0) {
        return 1;
    }

    char url[kMaxUrl];
    const char* args = reinterpret_cast<const char*>(arg_ptr);
    if (args == nullptr || args[0] == '\0') {
        if (prompt_line(url, sizeof(url)) == 0) {
            print_line("usage: browse <url>");
            return 1;
        }
    } else {
        strlcpy(url, args, sizeof(url));
    }

    return browse_url(url);
}
