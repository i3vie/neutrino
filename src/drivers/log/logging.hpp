#pragma once
#include <stddef.h>
#include <stdint.h>

enum class LogLevel : uint8_t {
    Debug,
    Info,
    Warn,
    Error,
};

void log_init();
void log_message(LogLevel level, const char* fmt, ...);
void log_set_console_enabled(bool enabled);
bool log_console_enabled();
size_t log_copy_recent(char* out, size_t max_len);
