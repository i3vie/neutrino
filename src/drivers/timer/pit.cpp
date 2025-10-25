#include "pit.hpp"

#include "arch/x86_64/io.hpp"
#include "../interrupts/pic.hpp"

namespace {

constexpr uint32_t PIT_INPUT_FREQUENCY = 1193182;
constexpr uint8_t PIT_CHANNEL0 = 0x40;
constexpr uint8_t PIT_COMMAND  = 0x43;

}  // namespace

namespace pit {

void init(uint32_t frequency_hz) {
    if (frequency_hz == 0) {
        frequency_hz = 100;
    }

    uint32_t divisor = PIT_INPUT_FREQUENCY / frequency_hz;
    if (divisor == 0) {
        divisor = 1;
    }

    outb(PIT_COMMAND, 0x36);
    outb(PIT_CHANNEL0, static_cast<uint8_t>(divisor & 0xFF));
    outb(PIT_CHANNEL0, static_cast<uint8_t>((divisor >> 8) & 0xFF));

    pic::set_mask(0, false);  // ensure timer IRQ is unmasked
}

}  // namespace pit

