#pragma once

#include "drivers/usb/usb_core.hpp"

namespace usb::hub {

void init();
bool probe_device(const usb::Device& device);

}  // namespace usb::hub
