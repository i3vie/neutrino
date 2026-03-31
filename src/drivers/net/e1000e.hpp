#pragma once

#include <stddef.h>
#include <stdint.h>

#include "descriptors.hpp"

namespace e1000e {

bool transmit_frame(void* context, const void* data, size_t length);

void register_driver();
void init();
void poll();

bool available();
const uint8_t* mac_address();
bool transmit(const void* data, size_t length);
bool get_debug_info(descriptor_defs::NetDeviceDebug& out);

}  // namespace e1000e
