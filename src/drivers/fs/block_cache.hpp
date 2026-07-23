#pragma once

#include "block_device.hpp"

namespace fs {
namespace block_cache {

void init();
void service_idle_flush();
bool flush_all();
void set_enabled(bool enabled);
bool enabled();

bool wrap_device(const BlockDevice& backing, BlockDevice& out_device);

}  // namespace block_cache
}  // namespace fs
