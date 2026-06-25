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
void drain();
void flush();
void set_paused(bool paused);
bool set_volume(uint8_t percent);
void get_status(size_t& queued_bytes, bool& running, bool& paused,
                uint8_t& volume);

}  // namespace hda
