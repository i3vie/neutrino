#pragma once

#include <stddef.h>
#include <stdint.h>

#include "descriptors.hpp"

namespace sensors {

using ReadFn = bool (*)(void* context, int64_t& value);

bool register_sensor(const char* name,
                     const char* adapter,
                     descriptor_defs::SensorKind kind,
                     descriptor_defs::SensorUnit unit,
                     ReadFn read,
                     void* context);
size_t count();
bool info(size_t index, descriptor_defs::SensorInfo& out);
bool read(size_t index, descriptor_defs::SensorSample& out);

}  // namespace sensors
