#pragma once
#include <stdint.h>
#include <stddef.h>

struct TSS {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1;
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

extern TSS tss;
extern "C" void set_rsp0(uint64_t rsp);
void init_tss();
void init_tss_for_cpu(TSS& tss_obj, uint8_t* stack, size_t stack_size);
