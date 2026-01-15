#include <stdint.h>

#include "../crt/syscall.hpp"

namespace {

constexpr const char* kShellPath = "binary/shell.elf";
constexpr const char* kCompositorPath = "binary/photon.elf";

[[noreturn]] void idle_loop() {
    for (;;) {
        yield();
    }
}

}  // namespace

int main(uint64_t, uint64_t) {
    child(kShellPath, nullptr, 0, nullptr);
    child(kCompositorPath, nullptr, 0, nullptr);
    idle_loop();
}
