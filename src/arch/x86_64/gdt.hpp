#pragma once
#include <stdint.h>
#include <stddef.h>

#define KERNEL_VIRT_BASE 0xffffffff80000000

extern "C" void gdt_install();
extern "C" uint16_t get_kernel_cs();
extern "C" uint16_t get_user_cs();

static constexpr uint16_t KERNEL_CS = 0x08;
static constexpr uint16_t KERNEL_DS = 0x10;
static constexpr uint16_t USER_CS   = 0x1B;
static constexpr uint16_t USER_DS   = 0x23;
static constexpr uint16_t TSS_SEL   = 0x30;
