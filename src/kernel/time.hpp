#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "neutrino_time.h"

namespace timekeeping {

bool init_from_rtc(uint32_t pit_frequency_hz);
void tick_pit();
bool snapshot(NeutrinoWallTime& out_time);
uint64_t tick_count();

}  // namespace timekeeping
