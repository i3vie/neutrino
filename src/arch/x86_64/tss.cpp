#include "tss.hpp"
#include "percpu.hpp"
#include "lib/mem.hpp"
static uint8_t tss_stack[65536] __attribute__((aligned(16)));

TSS tss __attribute__((aligned(16)));

extern "C" void set_rsp0(uint64_t rsp) {
    percpu::Cpu* cpu = percpu::current_cpu();
    if (cpu != nullptr) {
        cpu->tss.rsp0 = rsp;
    } else {
        tss.rsp0 = rsp;
    }
}

void init_tss_for_cpu(TSS& tss_obj, uint8_t* stack, size_t stack_size) {
    memset(&tss_obj, 0, sizeof(TSS));
    uint64_t stack_top = reinterpret_cast<uint64_t>(stack) + stack_size;
    stack_top &= ~0xFULL;
    tss_obj.rsp0 = stack_top;
    tss_obj.ist1 = tss_obj.ist2 = tss_obj.ist3 = tss_obj.ist4 =
        tss_obj.ist5 = tss_obj.ist6 = tss_obj.ist7 = 0;
    tss_obj.iomap_base = sizeof(TSS);
}

void init_tss() {
    init_tss_for_cpu(tss, tss_stack, sizeof(tss_stack));
}
