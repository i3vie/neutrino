#include "loader.hpp"

#include "drivers/log/logging.hpp"
#include "lib/mem.hpp"
#include "vm.hpp"

namespace {

constexpr uint8_t kDemoProgram[] = {
    0xB8, 0x00, 0x00, 0x00, 0x00,  // mov eax, 0
    0xBF, 0x44, 0x33, 0x22, 0x11,  // mov edi, 0x11223344
    0xBE, 0x88, 0x77, 0x66, 0x55,  // mov esi, 0x55667788
    0xBA, 0xCC, 0xBB, 0xAA, 0x99,  // mov edx, 0x99AABBCC
    0x0F, 0x05,                    // syscall
    0xEB, 0xE8                     // jmp back to program start
};
constexpr uint8_t kDemoProgram2[] = {
    0xB8, 0x00, 0x00, 0x00, 0x00,  // mov eax, 0
    0xBF, 0x11, 0x11, 0x11, 0x11,  // mov edi, 0x11111111
    0xBE, 0x22, 0x22, 0x22, 0x22,  // mov esi, 0x22222222
    0xBA, 0x33, 0x33, 0x33, 0x33,  // mov edx, 0x33333333
    0x0F, 0x05,                    // syscall
    0xEB, 0xE8                     // jmp back to program start
};

constexpr loader::ProgramImage kDemoImage = {
    kDemoProgram,
    sizeof(kDemoProgram),
    0,
};
constexpr loader::ProgramImage kDemoImage2 = {
    kDemoProgram2,
    sizeof(kDemoProgram2),
    0,
};

bool strings_equal(const char* a, const char* b) {
    if (a == nullptr || b == nullptr) {
        return false;
    }
    while (*a && *b) {
        if (*a != *b) {
            return false;
        }
        ++a;
        ++b;
    }
    return (*a == '\0') && (*b == '\0');
}

}  // namespace

namespace loader {

bool load_into_process(const ProgramImage& image, process::Process& proc) {
    uint64_t entry_point = 0;
    proc.code_region = vm::map_user_code(image.data, image.size,
                                         image.entry_offset, entry_point);
    if (proc.code_region.base == 0) {
        log_message(LogLevel::Error,
                    "Loader: failed to map user code for process %u",
                    static_cast<unsigned int>(proc.pid));
        return false;
    }

    proc.stack_region = vm::allocate_user_stack(16 * 1024);
    if (proc.stack_region.top == 0) {
        log_message(LogLevel::Error,
                    "Loader: failed to allocate stack for process %u",
                    static_cast<unsigned int>(proc.pid));
        return false;
    }

    proc.user_ip = entry_point;
    uint64_t aligned_top = (proc.stack_region.top - 16ull) & ~0xFull;
    proc.user_sp = aligned_top;
    proc.has_context = false;
    proc.state = process::State::Ready;
    return true;
}

}  // namespace loader
