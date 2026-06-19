#pragma once

#include <stddef.h>
#include <stdint.h>

namespace hda {

// Register the PCI class driver. The exposed PCM format is intentionally fixed:
// signed little-endian, 48 kHz, 16-bit, stereo (four bytes per frame).
void register_driver();
void init();

bool available();
size_t write_pcm(const void* data, size_t bytes);

}  // namespace hda
