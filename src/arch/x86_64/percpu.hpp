#pragma once

#include <stddef.h>
#include <stdint.h>

#include "arch/x86_64/tss.hpp"

namespace process {
struct Process;
}  // namespace process

namespace percpu {

constexpr size_t kMaxCpus = 16;
constexpr size_t kBootstrapStackSize = 0x4000;

struct Cpu {
    uint32_t lapic_id;
    uint32_t processor_id;
    uint32_t index;
    bool registered;
    TSS tss;
    alignas(16) uint8_t tss_stack[65536];
    alignas(16) uint8_t gdt_area[8 * 8];
    alignas(16) uint8_t bootstrap_stack[kBootstrapStackSize];
    process::Process* current_process;
};

void init_bsp(uint32_t lapic_id, uint32_t processor_id);
Cpu* register_cpu(uint32_t lapic_id, uint32_t processor_id);
Cpu* cpu_from_index(size_t index);
Cpu* current_cpu();
Cpu* find_by_lapic(uint32_t lapic_id);
size_t cpu_count();
void set_current_cpu(Cpu* cpu);
void setup_cpu_tss(Cpu& cpu);
void setup_cpu_gdt(Cpu& cpu);
void set_current_process(process::Process* proc);
process::Process* get_current_process();

}  // namespace percpu
