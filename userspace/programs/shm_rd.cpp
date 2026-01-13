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

bool wait_for_flag(volatile uint32_t* flag, int max_spins) {
    if (flag == nullptr) {
        return false;
    }
    for (int i = 0; i < max_spins; ++i) {
        if (*flag != 0) {
            return true;
        }
        yield();
    }
    return false;
}

}  // namespace

int main(uint64_t arg_ptr, uint64_t) {
    const char* name = reinterpret_cast<const char*>(arg_ptr);
    if (name == nullptr || name[0] == '\0') {
        name = kSharedName;
    }

    long console = descriptor_open(kDescConsole, 0);
    if (console < 0) {
        return 1;
    }

    uint32_t shm_handle = 0;
    SharedBlock* shared = nullptr;
    if (!open_shared_block(name, shm_handle, shared)) {
        log_line(console, "shm_rd: shared memory open failed");
        descriptor_close(static_cast<uint32_t>(console));
        return 1;
    }

    if (!wait_for_flag(&shared->data_ready, 1000000)) {
        log_line(console, "shm_rd: timed out waiting for data");
        descriptor_close(shm_handle);
        descriptor_close(static_cast<uint32_t>(console));
        return 1;
    }

    log_line(console, "shm_rd: received message:");
    log_line(console, shared->payload);
    shared->reader_done = 1;

    descriptor_close(shm_handle);
    descriptor_close(static_cast<uint32_t>(console));
    return 0;
}
