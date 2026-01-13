#include <stddef.h>
#include <stdint.h>

#include "descriptors.hpp"
#include "../crt/syscall.hpp"

namespace {

constexpr uint32_t kDescConsole =
    static_cast<uint32_t>(descriptor_defs::Type::Console);

constexpr size_t kSharedPayloadSize = 256;
constexpr const char kSharedName[] = "demo_shared";

struct SharedBlock {
    volatile uint32_t data_ready;
    volatile uint32_t reader_done;
    volatile uint32_t payload_length;
    char payload[kSharedPayloadSize];
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

void log_line(long console, const char* text) {
    if (console < 0 || text == nullptr) {
        return;
    }
    descriptor_write(static_cast<uint32_t>(console),
                     text,
                     str_len(text));
    descriptor_write(static_cast<uint32_t>(console), "\n", 1);
}

bool open_shared_block(const char* name,
                       uint32_t& handle_out,
                       SharedBlock*& block_out) {
    long handle = shared_memory_open(name, sizeof(SharedBlock));
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

}  // namespace

int main(uint64_t arg_ptr, uint64_t) {
    const char* name = reinterpret_cast<const char*>(arg_ptr);
    if (name == nullptr || name[0] == '\0') {
        name = kSharedName;
    }

    long console = descriptor_open(kDescConsole, 0);

    uint32_t shm_handle = 0;
    SharedBlock* shared = nullptr;
    if (!open_shared_block(name, shm_handle, shared)) {
        log_line(console, "shm_wr: shared memory open failed");
        if (console >= 0) {
            descriptor_close(static_cast<uint32_t>(console));
        }
        return 1;
    }

    shared->data_ready = 0;
    shared->reader_done = 0;

    const char* message = "Hello from shm_wr via shared memory!";
    str_copy(shared->payload, sizeof(shared->payload), message);
    shared->payload_length =
        static_cast<uint32_t>(str_len(shared->payload));
    shared->data_ready = 1;

    log_line(console, "shm_wr: wrote shared message");

    descriptor_close(shm_handle);
    if (console >= 0) {
        descriptor_close(static_cast<uint32_t>(console));
    }
    return 0;
}
