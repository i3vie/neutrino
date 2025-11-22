#include "percpu.hpp"

#include <stddef.h>
#include <stdint.h>

#include "arch/x86_64/gdt.hpp"
#include "arch/x86_64/tss.hpp"

namespace {

constexpr uint32_t kMsrGsBase = 0xC0000101;
constexpr uint32_t kMsrKernelGsBase = 0xC0000102;

percpu::Cpu g_cpus[percpu::kMaxCpus];
size_t g_cpu_count = 0;

inline void write_msr(uint32_t msr, uint64_t value) {
    uint32_t low = static_cast<uint32_t>(value & 0xFFFFFFFF);
    uint32_t high = static_cast<uint32_t>(value >> 32);
    asm volatile("wrmsr" : : "c"(msr), "a"(low), "d"(high));
}

inline uint64_t read_msr(uint32_t msr) {
    uint32_t low, high;
    asm volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return (static_cast<uint64_t>(high) << 32) | low;
}

}  // namespace

namespace percpu {

void setup_cpu_tss(Cpu& cpu) {
    init_tss_for_cpu(cpu.tss, cpu.tss_stack, sizeof(cpu.tss_stack));
}

void setup_cpu_gdt(Cpu& cpu) {
    gdt_install_for_cpu(&cpu.tss, cpu.gdt_area, sizeof(cpu.gdt_area));
}

Cpu* register_cpu(uint32_t lapic_id, uint32_t processor_id) {
    if (g_cpu_count >= kMaxCpus) {
        return nullptr;
    }
    Cpu& cpu = g_cpus[g_cpu_count++];
    cpu.lapic_id = lapic_id;
    cpu.processor_id = processor_id;
    cpu.index = static_cast<uint32_t>(g_cpu_count - 1);
    cpu.registered = false;
    cpu.current_process = nullptr;
    return &cpu;
}

Cpu* cpu_from_index(size_t index) {
    if (index >= g_cpu_count) {
        return nullptr;
    }
    return &g_cpus[index];
}

Cpu* current_cpu() {
    uint64_t base = read_msr(kMsrKernelGsBase);
    return reinterpret_cast<Cpu*>(base);
}

Cpu* find_by_lapic(uint32_t lapic_id) {
    for (size_t i = 0; i < g_cpu_count; ++i) {
        if (g_cpus[i].lapic_id == lapic_id) {
            return &g_cpus[i];
        }
    }
    return nullptr;
}

size_t cpu_count() {
    return g_cpu_count;
}

void set_current_cpu(Cpu* cpu) {
    if (cpu != nullptr) {
        write_msr(kMsrGsBase, reinterpret_cast<uint64_t>(cpu));
        write_msr(kMsrKernelGsBase, reinterpret_cast<uint64_t>(cpu));
    }
}

void init_bsp(uint32_t lapic_id, uint32_t processor_id) {
    Cpu* cpu = register_cpu(lapic_id, processor_id);
    if (cpu == nullptr) {
        return;
    }
    setup_cpu_tss(*cpu);
    setup_cpu_gdt(*cpu);
    set_current_cpu(cpu);
}

void set_current_process(process::Process* proc) {
    Cpu* cpu = current_cpu();
    if (cpu != nullptr) {
        cpu->current_process = proc;
    }
}

process::Process* get_current_process() {
    Cpu* cpu = current_cpu();
    return cpu ? cpu->current_process : nullptr;
}

}  // namespace percpu
