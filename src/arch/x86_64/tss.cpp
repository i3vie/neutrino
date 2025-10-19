#include "tss.hpp"
#include "lib/mem.hpp"

TSS tss __attribute__((aligned(16)));

extern "C" void set_rsp0(uint64_t rsp) {
    tss.rsp0 = rsp;
}
__attribute__((constructor)) static void init_tss() {
    tss.iomap_base = sizeof(TSS);
}
