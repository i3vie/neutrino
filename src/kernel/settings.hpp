#pragma once

#include <stddef.h>
#include <stdint.h>

#include "descriptors.hpp"

class Console;

namespace settings {

void set_storage_root(const char* root_mount_name);
bool load_from_disk();
bool save_to_disk();

const char* get_string(const char* key);
bool get_u32(const char* key, uint32_t& out);
bool set_string(const char* key, const char* value);
bool set_u32(const char* key, uint32_t value);

bool apply_console_preferences(Console& console);
void persist_console_scale(uint32_t scale);
void persist_console_font(const descriptor_defs::ConsoleFont& font,
                          const uint8_t* data);

}  // namespace settings
