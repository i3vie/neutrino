#include <stddef.h>
#include <stdint.h>

#include "descriptors.hpp"
#include "../crt/syscall.hpp"

namespace {

constexpr const char kSharedName[] = "demo_shared";
constexpr uint32_t kDescConsole =
    static_cast<uint32_t>(descriptor_defs::Type::Console);

struct SharedBlock {
    volatile uint32_t progress;
    volatile uint32_t ready;
    volatile uint32_t reader_done;
    char payload[64];
};

size_t str_len(const char* s) {
    size_t n = 0;
    if (s == nullptr) {
        return 0;
    }
    while (s[n] != '\0') {
        ++n;
    }
    return n;
}

void log_line(const char* text) {
    long console = descriptor_open(kDescConsole, 0);
    if (console < 0 || text == nullptr) {
        return;
    }
    descriptor_write(static_cast<uint32_t>(console), text, str_len(text));
    descriptor_write(static_cast<uint32_t>(console), "\n", 1);
    descriptor_close(static_cast<uint32_t>(console));
}

bool attach_shared_block(uint32_t& handle, SharedBlock*& block) {
    long fd = shared_memory_open(kSharedName, 0);
    if (fd < 0) {
        return false;
    }
    descriptor_defs::SharedMemoryInfo info{};
    if (shared_memory_get_info(static_cast<uint32_t>(fd), &info) != 0 ||
        info.base == 0 || info.length < sizeof(SharedBlock)) {
        descriptor_close(static_cast<uint32_t>(fd));
        return false;
    }
    handle = static_cast<uint32_t>(fd);
    block = reinterpret_cast<SharedBlock*>(info.base);
    return true;
}

int run_main(uint64_t /*arg*/, uint64_t /*flags*/) {
    log_line("reader: start");
    uint32_t shm = 0;
    SharedBlock* shared = nullptr;
    if (!attach_shared_block(shm, shared)) {
        log_line("reader: cannot attach shared block");
        return 1;
    }
    log_line("reader: attached shared block");
    for (int i = 0; i < 1000000; ++i) {
        if (shared->ready != 0) {
            break;
        }
        yield();
    }
    if (shared->ready == 0) {
        log_line("reader: timeout");
        descriptor_close(shm);
        return 1;
    }
    log_line("reader: observed ready");
    log_line("reader: got payload");
    log_line(shared->payload);
    shared->reader_done = 1;
    descriptor_close(shm);
    return 0;
}

}  // namespace

int main(uint64_t arg, uint64_t flags) {
    return run_main(arg, flags);
}
