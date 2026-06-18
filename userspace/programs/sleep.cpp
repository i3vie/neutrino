#include <stddef.h>
#include <stdint.h>
#include <neutrino.h>
#include "../crt/syscall.hpp"

namespace {

constexpr uint64_t kNanosecondsPerSecond = 1000000000ull;

uint64_t wall_time_ns(const NeutrinoWallTime& time) {
    return time.unix_seconds * kNanosecondsPerSecond + time.nanoseconds;
}

bool parse_duration_ns(const char* text, uint64_t& out) {
    if (text == nullptr || text[0] == '\0') {
        return false;
    }

    uint64_t seconds = 0;
    size_t i = 0;
    bool saw_digit = false;
    while (text[i] >= '0' && text[i] <= '9') {
        saw_digit = true;
        seconds = seconds * 10u + static_cast<uint64_t>(text[i] - '0');
        ++i;
    }

    uint64_t nanos = 0;
    uint64_t place = 100000000ull;
    if (text[i] == '.') {
        ++i;
        while (text[i] >= '0' && text[i] <= '9') {
            saw_digit = true;
            if (place > 0) {
                nanos += static_cast<uint64_t>(text[i] - '0') * place;
                place /= 10u;
            }
            ++i;
        }
    }

    if (!saw_digit || text[i] != '\0') {
        return false;
    }
    if (seconds > UINT64_MAX / kNanosecondsPerSecond) {
        return false;
    }

    uint64_t total = seconds * kNanosecondsPerSecond;
    if (UINT64_MAX - total < nanos) {
        return false;
    }
    out = total + nanos;
    return true;
}

bool sleep_until(uint64_t deadline_ns) {
    for (;;) {
        NeutrinoWallTime now{};
        if (!neutrino_get_time(&now)) {
            return false;
        }
        if (wall_time_ns(now) >= deadline_ns) {
            return true;
        }
        sleep_ms(1);
    }
}

}  // namespace

int main(uint64_t arg_ptr, uint64_t) {
    const char* args = reinterpret_cast<const char*>(arg_ptr);
    long console = neutrino_open_stdout();

    uint64_t duration_ns = 0;
    if (!parse_duration_ns(args, duration_ns)) {
        neutrino_write_line(console, "usage: sleep <seconds>");
        return 1;
    }

    NeutrinoWallTime start{};
    if (!neutrino_get_time(&start)) {
        neutrino_write_line(console, "sleep: time unavailable");
        return 1;
    }

    uint64_t start_ns = wall_time_ns(start);
    if (UINT64_MAX - start_ns < duration_ns) {
        neutrino_write_line(console, "sleep: duration too large");
        return 1;
    }

    if (!sleep_until(start_ns + duration_ns)) {
        neutrino_write_line(console, "sleep: time unavailable");
        return 1;
    }
    return 0;
}
