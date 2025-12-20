#include <stddef.h>
#include <stdint.h>

#include "descriptors.hpp"
#include "../crt/syscall.hpp"

namespace {

constexpr const char kSharedName[] = "demo_shared";
constexpr const char kWriterPath[] = "/IDE_PM_0/binary/SHM_WR~1.ELF";
constexpr const char kReaderPath[] = "/IDE_PM_0/binary/SHM_RE~1.ELF";

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

bool allocate_shared_block(uint32_t length,
                           uint32_t& handle,
                           SharedBlock*& block) {
    long fd = shared_memory_open(kSharedName, length);
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
    uint32_t shm = 0;
    SharedBlock* shared = nullptr;
    if (!allocate_shared_block(sizeof(SharedBlock), shm, shared)) {
        log_line("parent: failed to allocate shared memory");
        return 1;
    }
    shared->progress = 0;
    shared->ready = 0;
    shared->reader_done = 0;
    shared->payload[0] = '\0';
    log_line("parent: spawning writer and reader");

    long writer_pid = child(kWriterPath, nullptr, 0, nullptr);
    yield();  // give writer CPU immediately
    if (writer_pid < 0) {
        log_line("parent: writer spawn failed");
        descriptor_close(shm);
        return 1;
    }

    uint32_t last_progress = 0;
    for (int i = 0; i < 5000000; ++i) {
        if (shared->progress != last_progress) {
            last_progress = shared->progress;
            log_line("parent: writer progress changed");
        }
        if (shared->ready != 0) {
            break;
        }
        yield();
    }
    if (shared->ready == 0) {
        log_line("parent: writer did not signal ready (timeout)");
        descriptor_close(shm);
        return 1;
    }

    long reader_pid = child(kReaderPath, nullptr, 0, nullptr);
    yield();  // give reader CPU immediately
    if (reader_pid < 0) {
        log_line("parent: reader spawn failed");
        descriptor_close(shm);
        return 1;
    }

    for (int i = 0; i < 5000000; ++i) {
        if (shared->reader_done != 0) {
            log_line("parent: reader completed");
            descriptor_close(shm);
            return 0;
        }
        yield();
    }

    log_line("parent: reader did not complete (timeout)");
    descriptor_close(shm);
    return 1;
}

}  // namespace

int main(uint64_t arg, uint64_t flags) {
    return run_main(arg, flags);
}
