#pragma once

#include <stddef.h>
#include <stdint.h>

namespace virtio_net {

bool transmit_frame(void* context, const void* data, size_t length);

void register_driver();
void init();
void poll();

bool available();
const uint8_t* mac_address();
bool transmit(const void* data, size_t length);

}  // namespace virtio_net
