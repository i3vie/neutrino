#include "tss.hpp"
#include "lib/mem.hpp"
static uint8_t tss_stack[65536] __attribute__((aligned(16)));

TSS tss __attribute__((aligned(16)));

extern "C" void set_rsp0(uint64_t rsp) {
    tss.rsp0 = rsp;
}

void init_tss() {
    memset(&tss, 0, sizeof(TSS));
    tss.rsp0 = (uint64_t)&tss_stack[sizeof(tss_stack)];
    tss.rsp0 &= ~0xF;

    tss.ist1 = tss.ist2 = tss.ist3 = tss.ist4 = tss.ist5 = tss.ist6 = tss.ist7 = 0;

    tss.iomap_base = sizeof(TSS);
}
