#include <stddef.h>
#include <stdint.h>

#include "descriptors.hpp"
#include "../crt/syscall.hpp"

namespace {

constexpr uint32_t kDescConsole =
    static_cast<uint32_t>(descriptor_defs::Type::Console);

constexpr uint64_t kPipeReadable = 1ull << 0;
constexpr uint64_t kPipeWritable = 1ull << 1;

constexpr size_t kSharedPayloadSize = 256;
constexpr const char kSharedName[] = "demo_shared";

struct SharedBlock {
    volatile uint32_t data_ready;
    volatile uint32_t reader_done;
    volatile uint32_t payload_length;
    char payload[kSharedPayloadSize];
};

struct ChildArgs {
    char role[8];
    uint32_t pipe_id;
    char shared_name[64];
};

size_t str_len(const char* text) {
    if (text == nullptr) {
        return 0;
    }
    size_t len = 0;
    while (text[len] != '\0') {
        ++len;
    }
    return len;
}

void str_copy(char* dest, size_t dest_size, const char* src) {
    if (dest == nullptr || dest_size == 0) {
        return;
    }
    size_t idx = 0;
    if (src != nullptr) {
        while (src[idx] != '\0' && idx + 1 < dest_size) {
            dest[idx] = src[idx];
            ++idx;
        }
    }
    dest[idx] = '\0';
}

bool str_equal(const char* a, const char* b) {
    if (a == nullptr || b == nullptr) {
        return false;
    }
    while (*a != '\0' && *b != '\0') {
        if (*a != *b) {
            return false;
        }
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

void log_line(long console, const char* text) {
    if (console < 0 || text == nullptr) {
        return;
    }
    descriptor_write(static_cast<uint32_t>(console),
                     text,
                     str_len(text));
    descriptor_write(static_cast<uint32_t>(console), "\n", 1);
}

void child_log(const char* text) {
    long console = descriptor_open(kDescConsole, 0);
    if (console < 0) {
        return;
    }
    log_line(console, text);
    descriptor_close(static_cast<uint32_t>(console));
}

bool write_line(uint32_t pipe_handle, const char* text) {
    if (text == nullptr) {
        return false;
    }
    char buffer[256];
    size_t len = str_len(text);
    if (len + 1 >= sizeof(buffer)) {
        len = sizeof(buffer) - 2;
    }
    for (size_t i = 0; i < len; ++i) {
        buffer[i] = text[i];
    }
    buffer[len++] = '\n';
    long written = descriptor_write(pipe_handle, buffer, len);
    return written == static_cast<long>(len);
}

int64_t read_line(uint32_t pipe_handle, char* buffer, size_t buffer_size) {
    if (buffer == nullptr || buffer_size == 0) {
        return -1;
    }
    size_t total = 0;
    while (total + 1 < buffer_size) {
        char ch = 0;
        long res = descriptor_read(pipe_handle, &ch, 1);
        if (res <= 0) {
            return (total > 0) ? static_cast<int64_t>(total)
                               : static_cast<int64_t>(res);
        }
        if (ch == '\n') {
            break;
        }
        buffer[total++] = ch;
    }
    buffer[total] = '\0';
    return static_cast<int64_t>(total);
}

bool u32_to_string(uint32_t value, char* out, size_t out_size) {
    if (out == nullptr || out_size == 0) {
        return false;
    }
    char tmp[16];
    size_t idx = 0;
    do {
        tmp[idx++] = static_cast<char>('0' + (value % 10u));
        value /= 10u;
    } while (value != 0 && idx < sizeof(tmp));
    if (idx >= out_size) {
        return false;
    }
    size_t out_idx = 0;
    while (idx > 0 && out_idx + 1 < out_size) {
        out[out_idx++] = tmp[--idx];
    }
    out[out_idx] = '\0';
    return true;
}

bool string_to_u32(const char* text, uint32_t& out_value) {
    if (text == nullptr || text[0] == '\0') {
        return false;
    }
    uint32_t value = 0;
    for (size_t i = 0; text[i] != '\0'; ++i) {
        char ch = text[i];
        if (ch < '0' || ch > '9') {
            return false;
        }
        value = value * 10u + static_cast<uint32_t>(ch - '0');
    }
    out_value = value;
    return true;
}

bool format_child_args(const char* role,
                       uint32_t pipe_id,
                       const char* shared_name,
                       char* out,
                       size_t out_size) {
    if (role == nullptr || shared_name == nullptr ||
        out == nullptr || out_size == 0) {
        return false;
    }
    char id_buffer[16];
    if (!u32_to_string(pipe_id, id_buffer, sizeof(id_buffer))) {
        return false;
    }
    size_t role_len = str_len(role);
    size_t id_len = str_len(id_buffer);
    size_t name_len = str_len(shared_name);
    size_t needed = role_len + 1 + id_len + 1 + name_len + 1;
    if (needed > out_size) {
        return false;
    }
    size_t idx = 0;
    for (size_t i = 0; i < role_len; ++i) {
        out[idx++] = role[i];
    }
    out[idx++] = ':';
    for (size_t i = 0; i < id_len; ++i) {
        out[idx++] = id_buffer[i];
    }
    out[idx++] = ':';
    for (size_t i = 0; i < name_len; ++i) {
        out[idx++] = shared_name[i];
    }
    out[idx] = '\0';
    return true;
}

bool parse_child_args(const char* args, ChildArgs& cfg) {
    if (args == nullptr) {
        return false;
    }
    char buffer[128];
    str_copy(buffer, sizeof(buffer), args);
    char* first_colon = nullptr;
    char* second_colon = nullptr;
    for (size_t i = 0; buffer[i] != '\0'; ++i) {
        if (buffer[i] == ':') {
            if (first_colon == nullptr) {
                first_colon = &buffer[i];
            } else {
                second_colon = &buffer[i];
                break;
            }
        }
    }
    if (first_colon == nullptr || second_colon == nullptr) {
        return false;
    }
    *first_colon = '\0';
    *second_colon = '\0';
    if (!string_to_u32(first_colon + 1, cfg.pipe_id) ||
        cfg.pipe_id == 0) {
        return false;
    }
    str_copy(cfg.role, sizeof(cfg.role), buffer);
    str_copy(cfg.shared_name, sizeof(cfg.shared_name), second_colon + 1);
    return cfg.shared_name[0] != '\0';
}

bool build_self_exec_path(char* out, size_t out_size) {
    if (out == nullptr || out_size == 0) {
        return false;
    }
    char cwd[128];
    long len = getcwd(cwd, sizeof(cwd));
    if (len <= 0 || cwd[0] != '/') {
        return false;
    }
    const char suffix[] = "/binary/shm_demo.elf";
    size_t mount_len = 0;
    size_t idx = 1;
    while (cwd[idx] != '\0' && cwd[idx] != '/') {
        ++idx;
        ++mount_len;
    }
    if (mount_len == 0) {
        if (sizeof(suffix) > out_size) {
            return false;
        }
        str_copy(out, out_size, suffix);
        return true;
    }
    size_t needed = 1 + mount_len + sizeof(suffix);
    if (needed > out_size) {
        return false;
    }
    size_t out_idx = 0;
    out[out_idx++] = '/';
    for (size_t i = 0; i < mount_len; ++i) {
        out[out_idx++] = cwd[1 + i];
    }
    for (size_t i = 0; suffix[i] != '\0'; ++i) {
        out[out_idx++] = suffix[i];
    }
    out[out_idx] = '\0';
    return true;
}

bool open_handshake_pipe(uint32_t& handle_out, uint32_t& id_out) {
    long handle = pipe_open_new(kPipeReadable);
    if (handle < 0) {
        return false;
    }
    descriptor_defs::PipeInfo info{};
    if (pipe_get_info(static_cast<uint32_t>(handle), &info) != 0 ||
        info.id == 0) {
        descriptor_close(static_cast<uint32_t>(handle));
        return false;
    }
    handle_out = static_cast<uint32_t>(handle);
    id_out = info.id;
    return true;
}

bool attach_pipe(uint32_t pipe_id, uint64_t flags, uint32_t& handle_out) {
    long handle = pipe_open_existing(flags, pipe_id);
    if (handle < 0) {
        return false;
    }
    handle_out = static_cast<uint32_t>(handle);
    return true;
}

bool open_shared_block(const char* name,
                       size_t length,
                       uint32_t& handle_out,
                       SharedBlock*& block_out) {
    long handle = shared_memory_open(name, length);
    if (handle < 0) {
        return false;
    }
    descriptor_defs::SharedMemoryInfo info{};
    if (shared_memory_get_info(static_cast<uint32_t>(handle), &info) != 0 ||
        info.base == 0 || info.length < sizeof(SharedBlock)) {
        descriptor_close(static_cast<uint32_t>(handle));
        return false;
    }
    handle_out = static_cast<uint32_t>(handle);
    block_out = reinterpret_cast<SharedBlock*>(info.base);
    return true;
}

int run_writer(const ChildArgs& cfg) {
    uint32_t pipe_handle = 0;
    if (!attach_pipe(cfg.pipe_id, kPipeWritable, pipe_handle)) {
        child_log("shm_demo writer: pipe attach failed");
        return 1;
    }

    uint32_t shm_handle = 0;
    SharedBlock* shared = nullptr;
    if (!open_shared_block(cfg.shared_name, sizeof(SharedBlock),
                           shm_handle, shared)) {
        child_log("shm_demo writer: shared memory open failed");
        descriptor_close(pipe_handle);
        return 1;
    }

    shared->data_ready = 0;
    shared->reader_done = 0;
    shared->payload_length = 0;

    const char* message = "Hello from writer via shared memory!";
    str_copy(shared->payload, sizeof(shared->payload), message);
    shared->payload_length =
        static_cast<uint32_t>(str_len(shared->payload));

    if (!write_line(pipe_handle, "READY")) {
        child_log("shm_demo writer: failed to notify reader");
        descriptor_close(shm_handle);
        descriptor_close(pipe_handle);
        return 1;
    }

    child_log("shm_demo writer: message published");
    descriptor_close(shm_handle);
    descriptor_close(pipe_handle);
    return 0;
}

int run_reader(const ChildArgs& cfg) {
    uint32_t pipe_handle = 0;
    if (!attach_pipe(cfg.pipe_id, kPipeReadable, pipe_handle)) {
        child_log("shm_demo reader: pipe attach failed");
        return 1;
    }

    uint32_t shm_handle = 0;
    SharedBlock* shared = nullptr;
    if (!open_shared_block(cfg.shared_name, 0, shm_handle, shared)) {
        child_log("shm_demo reader: shared memory open failed");
        descriptor_close(pipe_handle);
        return 1;
    }

    char buffer[256];
    int64_t len = read_line(pipe_handle, buffer, sizeof(buffer));
    if (len <= 0 || !str_equal(buffer, "READY")) {
        child_log("shm_demo reader: unexpected signal");
        descriptor_close(shm_handle);
        descriptor_close(pipe_handle);
        return 1;
    }

    long console = descriptor_open(kDescConsole, 0);
    if (console >= 0) {
        log_line(console, "shm_demo reader: received message:");
        log_line(console, shared->payload);
        descriptor_close(static_cast<uint32_t>(console));
    }
    shared->data_ready = 0;
    shared->reader_done = 1;

    descriptor_close(shm_handle);
    descriptor_close(pipe_handle);
    return 0;
}

int run_parent() {
    long console = descriptor_open(kDescConsole, 0);
    if (console < 0) {
        return 1;
    }

    uint32_t shm_handle = 0;
    SharedBlock* shared = nullptr;
    if (!open_shared_block(kSharedName, sizeof(SharedBlock),
                           shm_handle, shared)) {
        log_line(console, "shm_demo: unable to allocate shared memory");
        return 1;
    }
    shared->data_ready = 0;
    shared->reader_done = 0;
    shared->payload_length = 0;

    uint32_t pipe_handle = 0;
    uint32_t pipe_id = 0;
    if (!open_handshake_pipe(pipe_handle, pipe_id)) {
        log_line(console, "shm_demo: unable to create pipe");
        descriptor_close(shm_handle);
        return 1;
    }
    log_line(console, "shm_demo: launching writer and reader");

    char writer_args[128];
    char reader_args[128];
    if (!format_child_args("writer", pipe_id, kSharedName,
                           writer_args, sizeof(writer_args)) ||
        !format_child_args("reader", pipe_id, kSharedName,
                           reader_args, sizeof(reader_args))) {
        log_line(console, "shm_demo: failed to build child args");
        descriptor_close(pipe_handle);
        descriptor_close(shm_handle);
        return 1;
    }

    char exec_path[128];
    if (!build_self_exec_path(exec_path, sizeof(exec_path))) {
        str_copy(exec_path, sizeof(exec_path), "/binary/shm_demo.elf");
    }

    child(exec_path, writer_args, 0, nullptr);
    child(exec_path, reader_args, 0, nullptr);

    bool reader_finished = false;
    for (int i = 0; i < 1000000; ++i) {
        if (shared->reader_done != 0) {
            reader_finished = true;
            break;
        }
        yield();
    }

    if (!reader_finished) {
        log_line(console, "shm_demo: timeout waiting for reader");
    } else {
        log_line(console, "shm_demo: reader consumed shared data");
    }

    descriptor_close(pipe_handle);
    descriptor_close(shm_handle);
    descriptor_close(static_cast<uint32_t>(console));
    return reader_finished ? 0 : 1;
}

int run_child_process(const char* args) {
    ChildArgs cfg{};
    if (!parse_child_args(args, cfg)) {
        child_log("shm_demo child: invalid args");
        return 1;
    }
    if (str_equal(cfg.role, "writer")) {
        return run_writer(cfg);
    }
    if (str_equal(cfg.role, "reader")) {
        return run_reader(cfg);
    }
    child_log("shm_demo child: unknown role");
    return 1;
}

}  // namespace

int main(uint64_t arg_ptr, uint64_t /*flags*/) {
    const char* args = reinterpret_cast<const char*>(arg_ptr);
    if (args != nullptr && args[0] != '\0') {
        return run_child_process(args);
    }
    return run_parent();
}
