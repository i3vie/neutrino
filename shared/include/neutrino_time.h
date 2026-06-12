#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct NeutrinoWallTime {
    uint64_t unix_seconds;
    uint32_t nanoseconds;
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    uint8_t weekday;
    uint8_t reserved[3];
};

#ifdef __cplusplus
}
#endif
