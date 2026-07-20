#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "neutrino_time.h"

namespace timekeeping {

bool init_from_rtc(uint32_t pit_frequency_hz);
void tick_pit();
bool snapshot(NeutrinoWallTime& out_time);
uint64_t tick_count();
uint64_t ticks_for_duration_ns(uint64_t duration_ns);
uint64_t nanoseconds_since_boot();

}  // namespace timekeeping
