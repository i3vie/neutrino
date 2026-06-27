#pragma once

#include <stddef.h>
#include <stdint.h>

#include "neutrino_time.h"

#ifndef __cplusplus
#include <stdbool.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

bool neutrino_parse_two_args(const char* args,
                             char* first,
                             size_t first_size,
                             char* second,
                             size_t second_size);
bool neutrino_copy_file(const char* source, const char* dest);
long neutrino_open_stdout();
void neutrino_write(long console, const char* text);
void neutrino_write_line(long console, const char* text);
bool neutrino_get_time(struct NeutrinoWallTime* out_time);
bool neutrino_sync();
bool neutrino_shutdown();

#ifdef __cplusplus
}
#endif
