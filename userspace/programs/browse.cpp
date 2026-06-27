#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <bearssl.h>

#include "descriptors.hpp"
#include "keyboard_scancode.hpp"
#include "neutrino.h"
#include "../crt/syscall.hpp"
#include "../helpers/http.hpp"
#include "../net/dns.hpp"
#include "../net/tcpd_protocol.hpp"

namespace {

constexpr uint32_t kDescConsole =
    static_cast<uint32_t>(descriptor_defs::Type::Console);
constexpr uint32_t kDescKeyboard =
    static_cast<uint32_t>(descriptor_defs::Type::Keyboard);
constexpr size_t kMaxUrl = 512;
constexpr size_t kIoBufferSize = 1024;
constexpr size_t kFileReadBufferSize = 8192;
constexpr size_t kRenderWidth = 78;
constexpr size_t kDefaultCols = 80;
constexpr size_t kDefaultRows = 25;
constexpr size_t kMaxLinks = 128;
constexpr size_t kMaxForms = 16;
constexpr size_t kMaxHiddenFields = 64;
constexpr size_t kMaxHistory = 16;
constexpr size_t kMaxLine = 240;
constexpr size_t kInputDisplayWidth = 24;
constexpr uint32_t kConnectWaitLimit = 200000;
constexpr uint32_t kMaxRedirects = 4;
constexpr uint32_t kDefaultFg = 0xFFFFFFFFu;
constexpr uint32_t kDefaultBg = 0x00000000u;
constexpr uint32_t kChromeFg = 0xFFB8D7FFu;
constexpr uint32_t kChromeBg = 0xFF102030u;
constexpr uint32_t kSecureFg = 0xFF7EE787u;
constexpr uint32_t kLinkFg = 0xFF8FD0FFu;
constexpr uint32_t kSelectedFg = 0xFF000000u;
constexpr uint32_t kSelectedBg = 0xFFFFD166u;
constexpr uint32_t kMutedFg = 0xFFAAAAAAu;

enum class TlsMode : uint8_t {
    None,
    Insecure,
    Verified,
    VerifiedNoTime,
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

struct TextLine {
    char text[kMaxLine];
    int16_t link_at[kMaxLine];
    size_t length;
};

enum class LinkKind : uint8_t {
    Anchor,
    TextInput,
    Submit,
    Button,
};

struct Link {
    LinkKind kind;
    char url[kMaxUrl];
    char label[96];
    char name[64];
    char value[128];
    size_t line;
    size_t start_column;
    size_t end_column;
    size_t form_index;
    bool password;
};

struct Form {
    char action[kMaxUrl];
    bool get;
};

struct HiddenField {
    size_t form_index;
    char name[64];
    char value[128];
};

struct BrowserDocument {
    TextLine* lines;
    size_t line_count;
    size_t line_capacity;
    Link links[kMaxLinks];
    size_t link_count;
    Form forms[kMaxForms];
    size_t form_count;
    HiddenField hidden_fields[kMaxHiddenFields];
    size_t hidden_field_count;
    int active_link;
    int current_form;
    bool in_link;
    char active_label[96];
    size_t active_label_length;
    uint32_t width;
};

struct BrowserSession {
    char current_url[kMaxUrl];
    char final_url[kMaxUrl];
    char next_url[kMaxUrl];
    char status_line[kMaxLine];
    char history[kMaxHistory][kMaxUrl];
};

struct TrustStore {
    bool attempted;
    bool loaded;
    br_x509_trust_anchor* anchors;
    size_t anchor_count;
    size_t anchor_capacity;
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
    BrowserDocument* document;
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
uint8_t g_file_read_buffer[kFileReadBufferSize];

using HttpResponseMeta = userspace::http::ResponseMeta;
using Scheme = userspace::http::Scheme;
using Url = userspace::http::Url;

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

void print_i32(int32_t value) {
    char buffer[16];
    size_t pos = 0;
    uint32_t magnitude = 0;
    if (value < 0) {
        buffer[pos++] = '-';
        magnitude = static_cast<uint32_t>(-(value + 1)) + 1;
    } else {
        magnitude = static_cast<uint32_t>(value);
    }
    char digits[10];
    size_t digit_count = 0;
    do {
        digits[digit_count++] = static_cast<char>('0' + (magnitude % 10));
        magnitude /= 10;
    } while (magnitude != 0 && digit_count < sizeof(digits));
    while (digit_count != 0 && pos < sizeof(buffer) - 1) {
        buffer[pos++] = digits[--digit_count];
    }
    buffer[pos] = '\0';
    print(buffer);
}

bool set_cursor(long console, uint32_t x, uint32_t y) {
    descriptor_defs::CursorPosition pos{x, y};
    return descriptor_set_property(
               static_cast<uint32_t>(console),
               static_cast<uint32_t>(descriptor_defs::Property::ConsoleCursor),
               &pos,
               sizeof(pos)) == 0;
}

bool clear_console(long console) {
    return descriptor_set_property(
               static_cast<uint32_t>(console),
               static_cast<uint32_t>(descriptor_defs::Property::ConsoleClear),
               nullptr,
               0) == 0;
}

bool set_console_color(long console, uint32_t fg, uint32_t bg) {
    descriptor_defs::ColorPair colors{fg, bg};
    return descriptor_set_property(
               static_cast<uint32_t>(console),
               static_cast<uint32_t>(descriptor_defs::Property::ConsoleColor),
               &colors,
               sizeof(colors)) == 0;
}

bool set_console_text_flags(long console, uint8_t flags) {
    return descriptor_set_property(
               static_cast<uint32_t>(console),
               static_cast<uint32_t>(descriptor_defs::Property::ConsoleTextFlags),
               &flags,
               sizeof(flags)) == 0;
}

bool query_console_size(uint32_t& cols, uint32_t& rows) {
    long console = process_get_standard_descriptor(1);
    if (console < 0) {
        console = descriptor_open(kDescConsole, 0);
    }
    if (console >= 0) {
        descriptor_defs::VtyInfo vty_info{};
        bool have_vty_info =
            descriptor_get_property(
                static_cast<uint32_t>(console),
                static_cast<uint32_t>(descriptor_defs::Property::VtyInfo),
                &vty_info,
                sizeof(vty_info)) == 0 &&
            vty_info.cols != 0 &&
            vty_info.rows != 0;
        if (process_get_standard_descriptor(1) < 0) {
            descriptor_close(static_cast<uint32_t>(console));
        }
        if (have_vty_info) {
            cols = vty_info.cols;
            rows = vty_info.rows;
            return true;
        }
    }

    long fb = framebuffer_open();
    if (fb < 0) {
        return false;
    }
    descriptor_defs::FramebufferInfo info{};
    bool ok = descriptor_get_property(
                  static_cast<uint32_t>(fb),
                  static_cast<uint32_t>(descriptor_defs::Property::FramebufferInfo),
                  &info,
                  sizeof(info)) == 0;
    descriptor_close(static_cast<uint32_t>(fb));
    if (!ok) {
        return false;
    }
    constexpr uint32_t cell_w = 16;
    constexpr uint32_t cell_h = 22;
    if (info.width >= cell_w) {
        cols = info.width / cell_w;
    }
    if (info.height >= cell_h) {
        rows = info.height / cell_h;
    }
    return true;
}

void print_padded(long console, const char* text, uint32_t cols) {
    size_t length = text != nullptr ? strlen(text) : 0;
    if (length > cols) {
        length = cols;
    }
    if (length != 0) {
        descriptor_write(static_cast<uint32_t>(console), text, length);
    }
    for (uint32_t i = static_cast<uint32_t>(length); i < cols; ++i) {
        descriptor_write(static_cast<uint32_t>(console), " ", 1);
    }
}

void print_spaces(long console, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        descriptor_write(static_cast<uint32_t>(console), " ", 1);
    }
}

char ascii_to_lower(char ch) {
    return userspace::http::to_lower(ch);
}

bool ascii_starts_with_case_insensitive(const char* text, const char* prefix) {
    return userspace::http::starts_with_ci(text, prefix);
}

int ascii_find_char(const char* text, char ch) {
    return userspace::http::find_char(text, ch);
}

void append_cstr(char* dest, const char* src, size_t capacity) {
    if (dest == nullptr || src == nullptr || capacity == 0) {
        return;
    }
    size_t length = strlen(dest);
    if (length >= capacity) {
        dest[capacity - 1] = '\0';
        return;
    }
    size_t i = 0;
    while (src[i] != '\0' && length + 1 < capacity) {
        dest[length++] = src[i++];
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

bool document_reserve_lines(BrowserDocument& doc, size_t required) {
    if (required <= doc.line_capacity) {
        return true;
    }
    size_t new_capacity = doc.line_capacity == 0 ? 128 : doc.line_capacity;
    while (new_capacity < required) {
        if (new_capacity > static_cast<size_t>(-1) / 2) {
            return false;
        }
        new_capacity *= 2;
    }
    auto* lines = static_cast<TextLine*>(
        map_anonymous(new_capacity * sizeof(TextLine), MAP_WRITE));
    if (lines == nullptr) {
        return false;
    }
    if (doc.lines != nullptr && doc.line_count != 0) {
        memcpy(lines, doc.lines, doc.line_count * sizeof(TextLine));
    }
    doc.lines = lines;
    doc.line_capacity = new_capacity;
    return true;
}

bool document_new_line(BrowserDocument& doc) {
    if (!document_reserve_lines(doc, doc.line_count + 1)) {
        return false;
    }
    TextLine& line = doc.lines[doc.line_count++];
    line.length = 0;
    line.text[0] = '\0';
    for (size_t i = 0; i < sizeof(line.link_at) / sizeof(line.link_at[0]); ++i) {
        line.link_at[i] = -1;
    }
    if (doc.in_link && doc.active_link >= 0 &&
        static_cast<size_t>(doc.active_link) < doc.link_count) {
        doc.links[doc.active_link].line = doc.line_count - 1;
        doc.links[doc.active_link].start_column = 0;
    }
    return true;
}

void document_init(BrowserDocument& doc, uint32_t width) {
    if (doc.lines != nullptr && doc.line_capacity != 0) {
        unmap(doc.lines, doc.line_capacity * sizeof(TextLine));
    }
    doc.lines = nullptr;
    doc.line_count = 0;
    doc.line_capacity = 0;
    doc.link_count = 0;
    doc.form_count = 0;
    doc.hidden_field_count = 0;
    doc.active_link = -1;
    doc.current_form = -1;
    doc.in_link = false;
    doc.active_label[0] = '\0';
    doc.active_label_length = 0;
    doc.width = width < 20 ? 20 : width;
    document_new_line(doc);
}

TextLine* document_current_line(BrowserDocument& doc) {
    if (doc.line_count == 0 && !document_new_line(doc)) {
        return nullptr;
    }
    return &doc.lines[doc.line_count - 1];
}

void document_append_char(BrowserDocument& doc, char ch) {
    if (ch == '\r') {
        return;
    }
    if (ch == '\n') {
        document_new_line(doc);
        return;
    }
    if (static_cast<unsigned char>(ch) < 0x20) {
        return;
    }
    TextLine* line = document_current_line(doc);
    if (line == nullptr) {
        return;
    }
    size_t wrap_width = doc.width;
    if (wrap_width >= kMaxLine) {
        wrap_width = kMaxLine - 1;
    }
    if (line->length >= wrap_width) {
        document_new_line(doc);
        line = document_current_line(doc);
        if (line == nullptr) {
            return;
        }
    }
    if (line->length + 1 >= sizeof(line->text)) {
        return;
    }
    line->link_at[line->length] =
        doc.in_link ? static_cast<int16_t>(doc.active_link) : -1;
    line->text[line->length++] = ch;
    line->text[line->length] = '\0';
    if (doc.in_link && doc.active_label_length + 1 < sizeof(doc.active_label)) {
        doc.active_label[doc.active_label_length++] = ch;
        doc.active_label[doc.active_label_length] = '\0';
    }
}

void document_append_text(BrowserDocument& doc, const char* text) {
    if (text == nullptr) {
        return;
    }
    for (size_t i = 0; text[i] != '\0'; ++i) {
        document_append_char(doc, text[i]);
    }
}

void document_trim_empty_tail(BrowserDocument& doc) {
    while (doc.line_count > 1 && doc.lines[doc.line_count - 1].length == 0) {
        --doc.line_count;
    }
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
    while (true) {
        long read = file_read(static_cast<uint32_t>(handle),
                              g_file_read_buffer,
                              sizeof(g_file_read_buffer));
        if (read < 0) {
            file_close(static_cast<uint32_t>(handle));
            print_line("browse: load_file read failed");
            return false;
        }
        if (read == 0) {
            break;
        }
        if (!buffer_append(out, g_file_read_buffer, static_cast<size_t>(read))) {
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
            print("browse: tcpd connect response not ok status=");
            print_i32(message.connect_response.status);
            print("\n");
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
    if (!load_file("/config/ssl/cacert.pem", pem_bytes) &&
        !load_file(".../config/ssl/cacert.pem", pem_bytes) &&
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
        NeutrinoWallTime now{};
        if (neutrino_get_time(&now)) {
            constexpr uint32_t kUnixEpochDays = 719528u;
            uint32_t days = kUnixEpochDays +
                            static_cast<uint32_t>(now.unix_seconds / 86400ull);
            uint32_t seconds =
                static_cast<uint32_t>(now.unix_seconds % 86400ull);
            br_x509_minimal_set_time(&stream.x509_minimal, days, seconds);
            print_line("browse: RTC time installed");
            stream.tls_mode = TlsMode::Verified;
        } else {
            br_x509_minimal_set_time_callback(&stream.x509_minimal,
                                              nullptr,
                                              ignore_certificate_time);
            print_line("browse: time callback installed");
            stream.tls_mode = TlsMode::VerifiedNoTime;
        }
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
    bool host_is_ip = userspace::http::parse_ipv4_literal(url.host, ip);
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

bool read_response_headers_adapter(void* context, char* out, size_t out_size) {
    return stream_read_line(*static_cast<Stream*>(context), out, out_size);
}

bool read_response_headers(Stream& stream, HttpResponseMeta& meta) {
    return userspace::http::read_response_headers(
        &stream,
        read_response_headers_adapter,
        meta);
}

void renderer_output_char(HtmlRenderer& renderer, char ch) {
    if (ch == '\r') {
        return;
    }
    if (ch == '\n') {
        if (renderer.document != nullptr) {
            document_append_char(*renderer.document, '\n');
        } else {
            print("\n");
        }
        renderer.column = 0;
        renderer.pending_space = false;
        renderer.last_was_newline = true;
        return;
    }
    size_t render_width = renderer.document != nullptr
                              ? renderer.document->width
                              : kRenderWidth;
    if (render_width < 20) {
        render_width = 20;
    }
    if (renderer.pending_space && renderer.column != 0) {
        if (renderer.column + 1 >= render_width) {
            if (renderer.document != nullptr) {
                document_append_char(*renderer.document, '\n');
            } else {
                print("\n");
            }
            renderer.column = 0;
        } else {
            if (renderer.document != nullptr) {
                document_append_char(*renderer.document, ' ');
            } else {
                print(" ");
            }
            ++renderer.column;
        }
    }
    renderer.pending_space = false;
    if (renderer.column >= render_width && ch != ' ') {
        if (renderer.document != nullptr) {
            document_append_char(*renderer.document, '\n');
        } else {
            print("\n");
        }
        renderer.column = 0;
    }
    if (renderer.document != nullptr) {
        document_append_char(*renderer.document, ch);
    } else {
        print_raw(&ch, 1);
    }
    ++renderer.column;
    renderer.last_was_newline = false;
    renderer.have_visible_text = true;
}

void renderer_emit_break(HtmlRenderer& renderer) {
    if (!renderer.last_was_newline) {
        if (renderer.document != nullptr) {
            document_append_char(*renderer.document, '\n');
        } else {
            print("\n");
        }
        renderer.column = 0;
        renderer.pending_space = false;
        renderer.last_was_newline = true;
    }
}

void renderer_emit_paragraph(HtmlRenderer& renderer) {
    if (!renderer.last_was_newline) {
        if (renderer.document != nullptr) {
            document_append_char(*renderer.document, '\n');
        } else {
            print("\n");
        }
    }
    if (renderer.document != nullptr) {
        document_append_char(*renderer.document, '\n');
    } else {
        print("\n");
    }
    renderer.column = 0;
    renderer.pending_space = false;
    renderer.last_was_newline = true;
}

bool extract_href(const char* tag, char* out, size_t out_size) {
    if (tag == nullptr || out == nullptr || out_size == 0) {
        return false;
    }
    out[0] = '\0';
    const char* cursor = tag;
    while (*cursor != '\0') {
        while (*cursor == ' ' || *cursor == '\t' || *cursor == '\n') {
            ++cursor;
        }
        if (ascii_to_lower(cursor[0]) == 'h' &&
            ascii_to_lower(cursor[1]) == 'r' &&
            ascii_to_lower(cursor[2]) == 'e' &&
            ascii_to_lower(cursor[3]) == 'f') {
            cursor += 4;
            while (*cursor == ' ' || *cursor == '\t') {
                ++cursor;
            }
            if (*cursor != '=') {
                continue;
            }
            ++cursor;
            while (*cursor == ' ' || *cursor == '\t') {
                ++cursor;
            }
            char quote = 0;
            if (*cursor == '\'' || *cursor == '"') {
                quote = *cursor++;
            }
            size_t length = 0;
            while (*cursor != '\0' &&
                   ((quote != 0 && *cursor != quote) ||
                    (quote == 0 && *cursor != ' ' && *cursor != '\t' &&
                     *cursor != '\n' && *cursor != '/'))) {
                if (length + 1 < out_size) {
                    out[length++] = *cursor;
                }
                ++cursor;
            }
            out[length] = '\0';
            return length != 0;
        }
        while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t' &&
               *cursor != '\n') {
            ++cursor;
        }
    }
    return false;
}

bool extract_attr(const char* tag, const char* attr, char* out, size_t out_size) {
    if (tag == nullptr || attr == nullptr || out == nullptr || out_size == 0) {
        return false;
    }
    out[0] = '\0';
    size_t attr_len = strlen(attr);
    const char* cursor = tag;
    while (*cursor != '\0') {
        while (*cursor == ' ' || *cursor == '\t' || *cursor == '\n') {
            ++cursor;
        }
        bool match = true;
        for (size_t i = 0; i < attr_len; ++i) {
            if (ascii_to_lower(cursor[i]) != ascii_to_lower(attr[i])) {
                match = false;
                break;
            }
        }
        char after_name = cursor[attr_len];
        if (match && (after_name == '=' || after_name == ' ' || after_name == '\t' ||
                      after_name == '\n' || after_name == '\0')) {
            cursor += attr_len;
            while (*cursor == ' ' || *cursor == '\t') {
                ++cursor;
            }
            if (*cursor != '=') {
                return false;
            }
            ++cursor;
            while (*cursor == ' ' || *cursor == '\t') {
                ++cursor;
            }
            char quote = 0;
            if (*cursor == '\'' || *cursor == '"') {
                quote = *cursor++;
            }
            size_t length = 0;
            while (*cursor != '\0' &&
                   ((quote != 0 && *cursor != quote) ||
                    (quote == 0 && *cursor != ' ' && *cursor != '\t' &&
                     *cursor != '\n' && *cursor != '/'))) {
                if (length + 1 < out_size) {
                    out[length++] = *cursor;
                }
                ++cursor;
            }
            out[length] = '\0';
            return length != 0;
        }
        while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t' &&
               *cursor != '\n') {
            ++cursor;
        }
    }
    return false;
}

void renderer_begin_link(HtmlRenderer& renderer, const char* href) {
    BrowserDocument* doc = renderer.document;
    if (doc == nullptr || href == nullptr || href[0] == '\0' ||
        doc->link_count >= kMaxLinks) {
        return;
    }
    size_t index = doc->link_count++;
    Link& link = doc->links[index];
    link.kind = LinkKind::Anchor;
    strlcpy(link.url, href, sizeof(link.url));
    link.label[0] = '\0';
    link.name[0] = '\0';
    link.value[0] = '\0';
    link.line = doc->line_count == 0 ? 0 : doc->line_count - 1;
    TextLine* line = document_current_line(*doc);
    link.start_column = line != nullptr ? line->length : 0;
    link.end_column = link.start_column;
    link.form_index = static_cast<size_t>(-1);
    link.password = false;
    doc->active_link = static_cast<int>(index);
    doc->in_link = true;
    doc->active_label[0] = '\0';
    doc->active_label_length = 0;
}

void renderer_end_link(HtmlRenderer& renderer) {
    BrowserDocument* doc = renderer.document;
    if (doc == nullptr || !doc->in_link || doc->active_link < 0 ||
        static_cast<size_t>(doc->active_link) >= doc->link_count) {
        return;
    }
    Link& link = doc->links[doc->active_link];
    if (doc->active_label_length != 0) {
        strlcpy(link.label, doc->active_label, sizeof(link.label));
    } else {
        strlcpy(link.label, link.url, sizeof(link.label));
    }
    doc->active_link = -1;
    doc->in_link = false;
    doc->active_label[0] = '\0';
    doc->active_label_length = 0;
}

void renderer_emit_control(HtmlRenderer& renderer,
                           LinkKind kind,
                           const char* name,
                           const char* value,
                           const char* label,
                           bool password) {
    BrowserDocument* doc = renderer.document;
    if (doc == nullptr || doc->link_count >= kMaxLinks) {
        return;
    }
    size_t index = doc->link_count++;
    Link& link = doc->links[index];
    link.kind = kind;
    link.url[0] = '\0';
    strlcpy(link.name, name != nullptr ? name : "", sizeof(link.name));
    strlcpy(link.value, value != nullptr ? value : "", sizeof(link.value));
    strlcpy(link.label, label != nullptr ? label : "", sizeof(link.label));
    link.form_index = doc->current_form >= 0
                          ? static_cast<size_t>(doc->current_form)
                          : static_cast<size_t>(-1);
    link.password = password;
    TextLine* line = document_current_line(*doc);
    link.line = doc->line_count == 0 ? 0 : doc->line_count - 1;
    link.start_column = line != nullptr ? line->length : 0;

    char display[48];
    display[0] = '\0';
    if (kind == LinkKind::TextInput) {
        strlcpy(display, "[", sizeof(display));
        size_t len = strlen(display);
        for (size_t i = 0; i < kInputDisplayWidth && len + 2 < sizeof(display); ++i) {
            char ch = ' ';
            if (value != nullptr && value[i] != '\0') {
                ch = password ? '*' : value[i];
            }
            display[len++] = ch;
        }
        display[len++] = ']';
        display[len] = '\0';
    } else {
        strlcpy(display, "[", sizeof(display));
        append_cstr(display,
                    (label != nullptr && label[0] != '\0') ? label : "Submit",
                    sizeof(display));
        append_cstr(display, "]", sizeof(display));
    }
    bool old_in_link = doc->in_link;
    int old_active_link = doc->active_link;
    size_t old_active_label_length = doc->active_label_length;
    doc->in_link = true;
    doc->active_link = static_cast<int>(index);
    for (size_t i = 0; display[i] != '\0'; ++i) {
        document_append_char(*doc, display[i]);
    }
    doc->in_link = old_in_link;
    doc->active_link = old_active_link;
    doc->active_label_length = old_active_label_length;
    doc->active_label[doc->active_label_length] = '\0';
    line = document_current_line(*doc);
    link.end_column = line != nullptr ? line->length : link.start_column;
}

void renderer_add_hidden_field(BrowserDocument& doc,
                               const char* name,
                               const char* value) {
    if (doc.current_form < 0 || name == nullptr || name[0] == '\0' ||
        doc.hidden_field_count >= kMaxHiddenFields) {
        return;
    }
    HiddenField& field = doc.hidden_fields[doc.hidden_field_count++];
    field.form_index = static_cast<size_t>(doc.current_form);
    strlcpy(field.name, name, sizeof(field.name));
    strlcpy(field.value, value != nullptr ? value : "", sizeof(field.value));
}

void renderer_finish_tag(HtmlRenderer& renderer) {
    renderer.tag_buffer[renderer.tag_length] = '\0';
    char raw_tag[sizeof(renderer.tag_buffer)];
    strlcpy(raw_tag, renderer.tag_buffer, sizeof(raw_tag));
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

    if (!closing && strcmp(tag, "a") == 0) {
        char href[kMaxUrl];
        if (extract_href(raw_tag, href, sizeof(href))) {
            renderer_begin_link(renderer, href);
        }
        return;
    }
    if (closing && strcmp(tag, "a") == 0) {
        renderer_end_link(renderer);
        return;
    }
    if (!closing && strcmp(tag, "form") == 0) {
        BrowserDocument* doc = renderer.document;
        if (doc != nullptr && doc->form_count < kMaxForms) {
            size_t index = doc->form_count++;
            Form& form = doc->forms[index];
            char method[16];
            form.get = !extract_attr(raw_tag, "method", method, sizeof(method)) ||
                       ascii_starts_with_case_insensitive(method, "get");
            if (!extract_attr(raw_tag, "action", form.action, sizeof(form.action))) {
                form.action[0] = '\0';
            }
            doc->current_form = static_cast<int>(index);
        }
        return;
    }
    if (closing && strcmp(tag, "form") == 0) {
        if (renderer.document != nullptr) {
            renderer.document->current_form = -1;
        }
        return;
    }
    if (!closing && strcmp(tag, "input") == 0) {
        char type[24];
        char name[64];
        char value[128];
        if (!extract_attr(raw_tag, "type", type, sizeof(type))) {
            strlcpy(type, "text", sizeof(type));
        }
        if (!extract_attr(raw_tag, "name", name, sizeof(name))) {
            name[0] = '\0';
        }
        if (!extract_attr(raw_tag, "value", value, sizeof(value))) {
            value[0] = '\0';
        }
        if (ascii_starts_with_case_insensitive(type, "hidden")) {
            if (renderer.document != nullptr) {
                renderer_add_hidden_field(*renderer.document, name, value);
            }
        } else if (ascii_starts_with_case_insensitive(type, "submit") ||
                   ascii_starts_with_case_insensitive(type, "button")) {
            renderer_emit_control(renderer,
                                  LinkKind::Submit,
                                  name,
                                  value,
                                  value[0] != '\0' ? value : "Submit",
                                  false);
        } else if (ascii_starts_with_case_insensitive(type, "text") ||
                   ascii_starts_with_case_insensitive(type, "search") ||
                   ascii_starts_with_case_insensitive(type, "password")) {
            renderer_emit_control(renderer,
                                  LinkKind::TextInput,
                                  name,
                                  value,
                                  name,
                                  ascii_starts_with_case_insensitive(type, "password"));
        }
        return;
    }
    if (!closing && strcmp(tag, "button") == 0) {
        char name[64];
        char value[128];
        if (!extract_attr(raw_tag, "name", name, sizeof(name))) {
            name[0] = '\0';
        }
        if (!extract_attr(raw_tag, "value", value, sizeof(value))) {
            value[0] = '\0';
        }
        renderer_emit_control(renderer, LinkKind::Button, name, value, "Submit", false);
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
            if (renderer.document != nullptr) {
                document_append_text(*renderer.document, "* ");
            } else {
                print("* ");
            }
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
    renderer.document = nullptr;
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

bool read_chunk_to_buffer(Stream& stream, Buffer& body, size_t size) {
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
        if (!buffer_append(body, buffer, static_cast<size_t>(got))) {
            return false;
        }
        remaining -= static_cast<size_t>(got);
    }
    uint8_t crlf[2];
    return stream_read(stream, crlf, 2) == 2;
}

bool read_body_to_buffer(Stream& stream, const HttpResponseMeta& meta, Buffer& body) {
    buffer_clear(body);
    if (!meta.is_text) {
        return true;
    }
    if (meta.chunked) {
        char line[128];
        for (;;) {
            if (!stream_read_line(stream, line, sizeof(line))) {
                return false;
            }
            size_t chunk_size = 0;
            if (!userspace::http::parse_hex_size(line, chunk_size)) {
                return false;
            }
            if (chunk_size == 0) {
                while (stream_read_line(stream, line, sizeof(line)) && line[0] != '\0') {
                }
                break;
            }
            if (!read_chunk_to_buffer(stream, body, chunk_size)) {
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
            if (!buffer_append(body, buffer, static_cast<size_t>(got))) {
                return false;
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
            if (!buffer_append(body, buffer, static_cast<size_t>(got))) {
                return false;
            }
        }
    }
    return true;
}

void render_body_to_document(const Buffer& body,
                             const HttpResponseMeta& meta,
                             BrowserDocument& doc) {
    if (!meta.is_text) {
        document_append_text(doc, "[non-text response omitted]");
        document_trim_empty_tail(doc);
        return;
    }
    HtmlRenderer renderer;
    renderer_init(renderer);
    renderer.document = &doc;
    if (meta.is_html) {
        renderer_process_html(renderer, body.data, body.size);
    } else {
        renderer_process_text(renderer, body.data, body.size);
    }
    if (!renderer.last_was_newline) {
        document_append_char(doc, '\n');
    }
    if (!renderer.have_visible_text) {
        document_append_text(doc, "[empty]");
    }
    document_trim_empty_tail(doc);
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

enum class BrowserKey : uint8_t {
    Char,
    Up,
    Down,
    Left,
    Right,
};

struct BrowserInput {
    BrowserKey key;
    char ch;
};

BrowserInput read_browser_input(uint32_t keyboard) {
    while (true) {
        descriptor_defs::KeyboardEvent events[8]{};
        long r = descriptor_read(keyboard, events, sizeof(events));
        if (r <= 0) {
            yield();
            continue;
        }
        size_t count = static_cast<size_t>(r) / sizeof(events[0]);
        for (size_t i = 0; i < count; ++i) {
            const auto& ev = events[i];
            if (!keyboard::is_pressed(ev)) {
                continue;
            }
            int32_t dx = 0;
            int32_t dy = 0;
            if (keyboard::is_arrow_key(ev, dx, dy)) {
                if (dy < 0) return {BrowserKey::Up, 0};
                if (dy > 0) return {BrowserKey::Down, 0};
                if (dx < 0) return {BrowserKey::Left, 0};
                if (dx > 0) return {BrowserKey::Right, 0};
            }
            if (keyboard::is_extended(ev)) {
                continue;
            }
            char ch = keyboard::scancode_to_char(ev.scancode, ev.mods);
            if (ch != 0) {
                return {BrowserKey::Char, ch};
            }
        }
    }
}

size_t prompt_line_on_status(uint32_t keyboard,
                             long console,
                             uint32_t row,
                             uint32_t cols,
                             const char* prompt,
                             char* out,
                             size_t out_capacity) {
    if (out == nullptr || out_capacity == 0) {
        return 0;
    }
    out[0] = '\0';
    set_console_color(console, kDefaultFg, kDefaultBg);
    set_cursor(console, 0, row);
    print_padded(console, "", cols);
    set_cursor(console, 0, row);
    print(prompt);
    size_t length = 0;
    while (true) {
        BrowserInput input = read_browser_input(keyboard);
        if (input.key != BrowserKey::Char) {
            continue;
        }
        char ch = input.ch;
        if (ch == '\n' || ch == '\r') {
            break;
        }
        if (ch == 27) {
            out[0] = '\0';
            return 0;
        }
        if (ch == '\b' || ch == 0x7F) {
            if (length != 0) {
                --length;
                out[length] = '\0';
                print("\b \b");
            }
            continue;
        }
        if (ch >= 0x20 && ch <= 0x7E && length + 1 < out_capacity) {
            out[length++] = ch;
            out[length] = '\0';
            print_raw(&ch, 1);
        }
    }
    return length;
}

int next_link_index(const BrowserDocument& doc, int selected, int direction) {
    if (doc.link_count == 0) {
        return -1;
    }
    if (selected < 0 || static_cast<size_t>(selected) >= doc.link_count) {
        return direction >= 0 ? 0 : static_cast<int>(doc.link_count - 1);
    }
    int count = static_cast<int>(doc.link_count);
    int next = selected + (direction >= 0 ? 1 : -1);
    if (next < 0) {
        next = count - 1;
    } else if (next >= count) {
        next = 0;
    }
    return next;
}

void clamp_view_to_selection(const BrowserDocument& doc,
                             size_t view_rows,
                             int selected_link,
                             size_t& scroll) {
    if (selected_link < 0 ||
        static_cast<size_t>(selected_link) >= doc.link_count ||
        view_rows == 0) {
        return;
    }
    size_t line = doc.links[selected_link].line;
    if (line < scroll) {
        scroll = line;
    } else if (line >= scroll + view_rows) {
        scroll = line - view_rows + 1;
    }
}

void render_url_bar(long console,
                    uint32_t row,
                    uint32_t cols,
                    const char* url,
                    TlsMode tls_mode) {
    set_cursor(console, 0, row);
    set_console_color(console, kChromeFg, kChromeBg);
    set_console_text_flags(console, 0);
    print_padded(console, "", cols);
    set_cursor(console, 0, row);

    uint32_t used = 0;
    if (tls_mode != TlsMode::None) {
        const char secure[] = "Secure";
        set_console_color(console, kSecureFg, kChromeBg);
        size_t secure_len = sizeof(secure) - 1;
        if (secure_len > cols) {
            secure_len = cols;
        }
        descriptor_write(static_cast<uint32_t>(console), secure, secure_len);
        used += static_cast<uint32_t>(secure_len);
        set_console_color(console, kChromeFg, kChromeBg);
        if (used < cols) {
            const char gap[] = " ";
            descriptor_write(static_cast<uint32_t>(console), gap, 1);
            ++used;
        }
    }

    char status[kMaxLine];
    status[0] = '\0';
    if (url != nullptr && url[0] != '\0') {
        append_cstr(status, url, sizeof(status));
    }

    if (used < cols) {
        size_t available = cols - used;
        size_t length = strlen(status);
        if (length > available) {
            length = available;
        }
        if (length != 0) {
            descriptor_write(static_cast<uint32_t>(console), status, length);
            used += static_cast<uint32_t>(length);
        }
    }
    if (used < cols) {
        print_spaces(console, cols - used);
    }
    set_console_color(console, kDefaultFg, kDefaultBg);
    set_console_text_flags(console, 0);
}

void render_link_bar(long console,
                     uint32_t row,
                     uint32_t cols,
                     const BrowserDocument& doc,
                     int selected_link) {
    set_cursor(console, 0, row);
    set_console_color(console, kChromeFg, kChromeBg);
    set_console_text_flags(console, 0);
    char status[kMaxLine];
    status[0] = '\0';
    if (selected_link >= 0 && static_cast<size_t>(selected_link) < doc.link_count) {
        const Link& link = doc.links[selected_link];
        if (link.url[0] != '\0') {
            append_cstr(status, link.url, sizeof(status));
        }
    }
    print_padded(console, status, cols);
    set_console_color(console, kDefaultFg, kDefaultBg);
    set_console_text_flags(console, 0);
}

void render_shortcut_legend(long console, uint32_t row, uint32_t cols) {
    set_cursor(console, 0, row);
    set_console_color(console, kChromeFg, kChromeBg);
    set_console_text_flags(console, 0);
    char legend[kMaxLine];
    strlcpy(legend,
            "^Q Quit  ^G Open URL  Enter Follow/Open  ^B Back  ^R Reload  Arrows Move",
            sizeof(legend));
    print_padded(console, legend, cols);
    set_console_color(console, kDefaultFg, kDefaultBg);
    set_console_text_flags(console, 0);
}

void render_document_line(long console,
                          const TextLine& line,
                          const BrowserDocument& doc,
                          int selected_link,
                          uint32_t cols) {
    size_t limit = line.length;
    if (limit > cols) {
        limit = cols;
    }
    size_t pos = 0;
    while (pos < limit) {
        int16_t link = line.link_at[pos];
        bool selected = link >= 0 && link == selected_link;
        if (selected) {
            set_console_color(console, kSelectedFg, kSelectedBg);
            set_console_text_flags(console, descriptor_defs::kTextCellUnderline);
        } else if (link >= 0) {
            set_console_color(console, kLinkFg, kDefaultBg);
            set_console_text_flags(console, descriptor_defs::kTextCellUnderline);
        } else {
            set_console_color(console, kDefaultFg, kDefaultBg);
            set_console_text_flags(console, 0);
        }

        size_t end = pos + 1;
        while (end < limit && line.link_at[end] == link) {
            ++end;
        }
        if (link >= 0 && static_cast<size_t>(link) < doc.link_count &&
            doc.links[link].kind == LinkKind::TextInput) {
            char display[48];
            size_t len = 0;
            display[len++] = '[';
            const Link& item = doc.links[link];
            for (size_t i = 0; i < kInputDisplayWidth && len + 2 < sizeof(display); ++i) {
                char ch = ' ';
                if (item.value[i] != '\0') {
                    ch = item.password ? '*' : item.value[i];
                }
                display[len++] = ch;
            }
            display[len++] = ']';
            display[len] = '\0';
            descriptor_write(static_cast<uint32_t>(console),
                             display,
                             strlen(display));
        } else {
            descriptor_write(static_cast<uint32_t>(console),
                             line.text + pos,
                             end - pos);
        }
        pos = end;
    }
    set_console_text_flags(console, 0);
    set_console_color(console, kDefaultFg, kDefaultBg);
    if (limit < cols) {
        print_spaces(console, cols - static_cast<uint32_t>(limit));
    }
}

void render_browser_screen(long console,
                           const BrowserDocument& doc,
                           const char* url,
                           int status_code,
                           size_t scroll,
                           int selected_link,
                           TlsMode tls_mode,
                           uint32_t cols,
                           uint32_t rows) {
    (void)status_code;
    if (rows < 4) {
        rows = 4;
    }
    clear_console(console);
    size_t view_rows = rows > 3 ? rows - 3 : 1;
    for (size_t i = 0; i < view_rows; ++i) {
        size_t line_index = scroll + i;
        set_cursor(console, 0, static_cast<uint32_t>(i));
        if (line_index < doc.line_count) {
            const TextLine& line = doc.lines[line_index];
            render_document_line(console, line, doc, selected_link, cols);
        } else {
            set_console_color(console, kMutedFg, kDefaultBg);
            set_console_text_flags(console, 0);
            print_padded(console, "", cols);
        }
    }
    set_console_text_flags(console, 0);
    render_url_bar(console, rows - 3, cols, url, tls_mode);
    render_link_bar(console, rows - 2, cols, doc, selected_link);
    render_shortcut_legend(console, rows - 1, cols);
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

bool load_url_document(const char* initial_url,
                       Stream& stream,
                       char* final_url,
                       size_t final_url_size,
                       BrowserDocument& doc,
                       Buffer& body,
                       int& status_code,
                       TlsMode& tls_mode,
                       uint32_t width) {
    char current_url[kMaxUrl];
    strlcpy(current_url, initial_url, sizeof(current_url));

    for (uint32_t redirect = 0; redirect <= kMaxRedirects; ++redirect) {
        Url url{};
        if (!userspace::http::parse_url(
                current_url,
                url,
                userspace::http::UrlParseMode::DefaultHttps)) {
            print_line("browse: invalid URL");
            return false;
        }

        TcpConnection tcp{};
        for (size_t i = 0; i < sizeof(stream); ++i) {
            reinterpret_cast<uint8_t*>(&stream)[i] = 0;
        }
        stream.tcp = &tcp;
        stream.tls = (url.scheme == Scheme::Https);
        if (!stream_connect(stream, url)) {
            print_line("browse: connect failed");
            return false;
        }
        if (!send_http_request(stream, url)) {
            stream_close(stream);
            print_line("browse: request failed");
            return false;
        }

        HttpResponseMeta meta{};
        if (!read_response_headers(stream, meta)) {
            stream_close(stream);
            print_line("browse: failed to read response");
            return false;
        }

        if (is_redirect_status(meta.status_code) && meta.location[0] != '\0') {
            char next_url[kMaxUrl];
            if (!userspace::http::build_redirect_url(
                    url,
                    meta.location,
                    next_url,
                    sizeof(next_url))) {
                stream_close(stream);
                print_line("browse: redirect URL too long");
                return false;
            }
            stream_close(stream);
            print("redirect -> ");
            print_line(next_url);
            strlcpy(current_url, next_url, sizeof(current_url));
            continue;
        }

        if (!read_body_to_buffer(stream, meta, body)) {
            stream_close(stream);
            print_line("browse: failed to read body");
            return false;
        }
        document_init(doc, width);
        render_body_to_document(body, meta, doc);
        status_code = meta.status_code;
        tls_mode = stream.tls_mode;
        strlcpy(final_url, current_url, final_url_size);
        stream_close(stream);
        return true;
    }

    print_line("browse: too many redirects");
    return false;
}

bool resolve_link_url(const char* current_url,
                      const char* link_url,
                      char* out,
                      size_t out_size) {
    if (link_url == nullptr || link_url[0] == '\0') {
        return false;
    }
    if (ascii_starts_with_case_insensitive(link_url, "mailto:") ||
        ascii_starts_with_case_insensitive(link_url, "javascript:") ||
        link_url[0] == '#') {
        return false;
    }
    Url current{};
    if (!userspace::http::parse_url(
            current_url,
            current,
            userspace::http::UrlParseMode::DefaultHttps)) {
        return false;
    }
    return userspace::http::build_redirect_url(current, link_url, out, out_size);
}

char hex_digit(uint8_t value) {
    return value < 10 ? static_cast<char>('0' + value)
                      : static_cast<char>('A' + (value - 10));
}

bool append_url_encoded(char* out, size_t out_size, size_t& length, const char* text) {
    if (out == nullptr || text == nullptr || out_size == 0) {
        return false;
    }
    for (size_t i = 0; text[i] != '\0'; ++i) {
        unsigned char ch = static_cast<unsigned char>(text[i]);
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' ||
            ch == '.' || ch == '~') {
            if (length + 1 >= out_size) return false;
            out[length++] = static_cast<char>(ch);
        } else if (ch == ' ') {
            if (length + 1 >= out_size) return false;
            out[length++] = '+';
        } else {
            if (length + 3 >= out_size) return false;
            out[length++] = '%';
            out[length++] = hex_digit(static_cast<uint8_t>(ch >> 4));
            out[length++] = hex_digit(static_cast<uint8_t>(ch & 0x0F));
        }
    }
    out[length] = '\0';
    return true;
}

bool append_query_pair(char* out,
                       size_t out_size,
                       size_t& length,
                       bool& first,
                       const char* name,
                       const char* value) {
    if (name == nullptr || name[0] == '\0') {
        return true;
    }
    if (length + 1 >= out_size) {
        return false;
    }
    out[length++] = first ? '?' : '&';
    first = false;
    out[length] = '\0';
    return append_url_encoded(out, out_size, length, name) &&
           length + 1 < out_size &&
           (out[length++] = '=', out[length] = '\0', true) &&
           append_url_encoded(out, out_size, length, value != nullptr ? value : "");
}

bool string_equals(const char* a, const char* b) {
    if (a == nullptr || b == nullptr) {
        return false;
    }
    return strcmp(a, b) == 0;
}

bool submit_control_name_exists(const BrowserDocument& doc,
                                size_t form_index,
                                const char* name) {
    if (name == nullptr || name[0] == '\0') {
        return false;
    }
    for (size_t i = 0; i < doc.link_count; ++i) {
        const Link& link = doc.links[i];
        if (link.form_index == form_index &&
            (link.kind == LinkKind::Submit || link.kind == LinkKind::Button) &&
            string_equals(link.name, name)) {
            return true;
        }
    }
    return false;
}

bool build_form_submit_url(const char* current_url,
                           const BrowserDocument& doc,
                           const Link& submit,
                           char* out,
                           size_t out_size) {
    if (submit.form_index >= doc.form_count) {
        return false;
    }
    const Form& form = doc.forms[submit.form_index];
    if (!form.get) {
        return false;
    }
    const char* action = form.action[0] != '\0' ? form.action : current_url;
    if (!resolve_link_url(current_url, action, out, out_size)) {
        return false;
    }
    size_t length = strlen(out);
    bool first = ascii_find_char(out, '?') < 0;
    for (size_t i = 0; i < doc.hidden_field_count; ++i) {
        const HiddenField& field = doc.hidden_fields[i];
        if (field.form_index != submit.form_index) {
            continue;
        }
        if (submit_control_name_exists(doc, submit.form_index, field.name)) {
            continue;
        }
        if (!append_query_pair(out, out_size, length, first, field.name, field.value)) {
            return false;
        }
    }
    for (size_t i = 0; i < doc.link_count; ++i) {
        const Link& link = doc.links[i];
        if (link.form_index != submit.form_index) {
            continue;
        }
        if (link.kind == LinkKind::TextInput) {
            if (!append_query_pair(out, out_size, length, first, link.name, link.value)) {
                return false;
            }
        } else if ((link.kind == LinkKind::Submit || link.kind == LinkKind::Button) &&
                   &link != &submit &&
                   link.name[0] != '\0' &&
                   string_equals(link.name, submit.name)) {
            continue;
        }
    }
    if (submit.name[0] != '\0') {
        return append_query_pair(out, out_size, length, first, submit.name, submit.value);
    }
    return true;
}

int browse_url(const char* initial_url) {
    uint32_t cols = kDefaultCols;
    uint32_t rows = kDefaultRows;
    query_console_size(cols, rows);
    if (cols > kMaxLine - 1) {
        cols = kMaxLine - 1;
    }
    if (cols < 40) {
        cols = 40;
    }
    long keyboard = descriptor_open(kDescKeyboard, 0);
    if (keyboard < 0) {
        print_line("browse: failed to open keyboard");
        return 1;
    }
    auto* stream = static_cast<Stream*>(map_anonymous(sizeof(Stream), MAP_WRITE));
    if (stream == nullptr) {
        descriptor_close(static_cast<uint32_t>(keyboard));
        print_line("browse: failed to allocate TLS state");
        return 1;
    }
    auto* doc = static_cast<BrowserDocument*>(
        map_anonymous(sizeof(BrowserDocument), MAP_WRITE));
    if (doc == nullptr) {
        descriptor_close(static_cast<uint32_t>(keyboard));
        print_line("browse: failed to allocate document state");
        return 1;
    }
    auto* session = static_cast<BrowserSession*>(
        map_anonymous(sizeof(BrowserSession), MAP_WRITE));
    if (session == nullptr) {
        descriptor_close(static_cast<uint32_t>(keyboard));
        print_line("browse: failed to allocate browser state");
        return 1;
    }

    strlcpy(session->current_url, initial_url, sizeof(session->current_url));
    size_t history_count = 0;

    Buffer body{};
    int status_code = 0;
    TlsMode tls_mode = TlsMode::None;
    size_t scroll = 0;
    int selected_link = -1;

    while (true) {
        clear_console(g_console);
        set_cursor(g_console, 0, 0);
        set_console_color(g_console, kChromeFg, kChromeBg);
        strlcpy(session->status_line, "loading ", sizeof(session->status_line));
        append_cstr(session->status_line,
                    session->current_url,
                    sizeof(session->status_line));
        print_padded(g_console, session->status_line, cols);
        set_console_color(g_console, kDefaultFg, kDefaultBg);

        if (!load_url_document(session->current_url,
                               *stream,
                               session->final_url,
                               sizeof(session->final_url),
                               *doc,
                               body,
                               status_code,
                               tls_mode,
                               cols)) {
            set_cursor(g_console, 0, rows > 1 ? rows - 1 : 0);
            set_console_color(g_console, kDefaultFg, kDefaultBg);
            print_padded(g_console, "load failed; press b for back or q to quit", cols);
            BrowserInput input = read_browser_input(static_cast<uint32_t>(keyboard));
            if (input.key == BrowserKey::Char && input.ch == 'b' && history_count != 0) {
                strlcpy(session->current_url,
                        session->history[--history_count],
                        sizeof(session->current_url));
                continue;
            }
            if (input.key == BrowserKey::Char && input.ch == 'q') {
                descriptor_close(static_cast<uint32_t>(keyboard));
                return 1;
            }
            continue;
        }
        strlcpy(session->current_url,
                session->final_url,
                sizeof(session->current_url));
        scroll = 0;
        selected_link = doc->link_count != 0 ? 0 : -1;
        size_t view_rows = rows > 3 ? rows - 3 : 1;

        bool reload = false;
        while (!reload) {
            render_browser_screen(g_console,
                                  *doc,
                                  session->current_url,
                                  status_code,
                                  scroll,
                                  selected_link,
                                  tls_mode,
                                  cols,
                                  rows);

            BrowserInput input = read_browser_input(static_cast<uint32_t>(keyboard));
            if (input.key == BrowserKey::Up) {
                if (scroll > 0) {
                    --scroll;
                }
                continue;
            }
            if (input.key == BrowserKey::Down) {
                if (scroll + view_rows < doc->line_count) {
                    ++scroll;
                }
                continue;
            }
            if (input.key == BrowserKey::Left) {
                selected_link = next_link_index(*doc, selected_link, -1);
                clamp_view_to_selection(*doc, view_rows, selected_link, scroll);
                continue;
            }
            if (input.key == BrowserKey::Right) {
                selected_link = next_link_index(*doc, selected_link, 1);
                clamp_view_to_selection(*doc, view_rows, selected_link, scroll);
                continue;
            }
            if (input.key != BrowserKey::Char) {
                continue;
            }
            char ch = input.ch;
            if (ch == 'q') {
                descriptor_close(static_cast<uint32_t>(keyboard));
                return 0;
            }
            if (ch == 'j' || ch == '\t') {
                selected_link = next_link_index(*doc, selected_link, 1);
                clamp_view_to_selection(*doc, view_rows, selected_link, scroll);
            } else if (ch == 'k') {
                selected_link = next_link_index(*doc, selected_link, -1);
                clamp_view_to_selection(*doc, view_rows, selected_link, scroll);
            } else if (ch == ' ' || ch == 'f') {
                size_t step = view_rows > 1 ? view_rows - 1 : 1;
                if (scroll + step < doc->line_count) {
                    scroll += step;
                }
            } else if (ch == 'u') {
                size_t step = view_rows > 1 ? view_rows - 1 : 1;
                scroll = scroll > step ? scroll - step : 0;
            } else if (ch == 'r') {
                reload = true;
            } else if (ch == 'b') {
                if (history_count != 0) {
                    strlcpy(session->current_url,
                            session->history[--history_count],
                            sizeof(session->current_url));
                    reload = true;
                }
            } else if (ch == 'g') {
                if (prompt_line_on_status(static_cast<uint32_t>(keyboard),
                                          g_console,
                                          rows - 1,
                                          cols,
                                          "url: ",
                                          session->next_url,
                                          sizeof(session->next_url)) != 0) {
                    if (history_count < kMaxHistory) {
                        strlcpy(session->history[history_count++],
                                session->current_url,
                                kMaxUrl);
                    }
                    strlcpy(session->current_url,
                            session->next_url,
                            sizeof(session->current_url));
                    reload = true;
                }
            } else if ((ch == '\n' || ch == '\r') &&
                       selected_link >= 0 &&
                       static_cast<size_t>(selected_link) < doc->link_count) {
                Link& selected = doc->links[selected_link];
                bool navigate = false;
                if (selected.kind == LinkKind::TextInput) {
                    if (prompt_line_on_status(static_cast<uint32_t>(keyboard),
                                              g_console,
                                              rows - 1,
                                              cols,
                                              "value: ",
                                              selected.value,
                                              sizeof(selected.value)) != 0) {
                        reload = false;
                    }
                } else if (selected.kind == LinkKind::Submit ||
                           selected.kind == LinkKind::Button) {
                    navigate = build_form_submit_url(session->current_url,
                                                     *doc,
                                                     selected,
                                                     session->next_url,
                                                     sizeof(session->next_url));
                } else {
                    navigate = resolve_link_url(session->current_url,
                                                selected.url,
                                                session->next_url,
                                                sizeof(session->next_url));
                }
                if (navigate) {
                    if (history_count < kMaxHistory) {
                        strlcpy(session->history[history_count++],
                                session->current_url,
                                kMaxUrl);
                    }
                    strlcpy(session->current_url,
                            session->next_url,
                            sizeof(session->current_url));
                    reload = true;
                }
            }
        }
    }
}

}  // namespace

int main(uint64_t arg_ptr, uint64_t) {
    g_console = process_get_standard_descriptor(1);
    if (g_console < 0) {
        g_console = descriptor_open(kDescConsole, 0);
    }
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
