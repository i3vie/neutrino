#include "smp.hpp"

#include <stdint.h>

#include "arch/x86_64/gdt.hpp"
#include "arch/x86_64/idt.hpp"
#include "arch/x86_64/percpu.hpp"
#include "arch/x86_64/memory/paging.hpp"
#include "arch/x86_64/lapic.hpp"
#include "kernel/scheduler.hpp"
#include "drivers/limine/limine_requests.hpp"
#include "drivers/log/logging.hpp"

namespace smp {
namespace {

static size_t g_online_cpus = 1;
static size_t g_cpu_count = 1;

}  // namespace

extern "C" void smp_ap_entry(struct LIMINE_MP(info)* info) {
    auto* cpu = reinterpret_cast<percpu::Cpu*>(info->extra_argument);
    if (cpu != nullptr) {
        uint8_t* stack_top = cpu->bootstrap_stack + percpu::kBootstrapStackSize;
        asm volatile("mov %0, %%rsp\n"
                     "xor %%rbp, %%rbp\n"
                     :
                     : "r"(stack_top)
                     : "memory");
        // Ensure APs use the kernel page tables.
        uint64_t cr3 = paging_cr3();
        if (cr3 != 0) {
            asm volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
        }
        percpu::set_current_cpu(cpu);
        percpu::setup_cpu_tss(*cpu);
        percpu::setup_cpu_gdt(*cpu);
    }

    idt_install();

    lapic::setup_timer(0x40, 10'000'000);

    __atomic_fetch_add(&g_online_cpus, 1, __ATOMIC_SEQ_CST);
    log_message(LogLevel::Info,
                "SMP: AP online (processor_id=%u lapic_id=%u)",
                info->processor_id,
                info->lapic_id);

    scheduler::run_cpu();
}

void init() {
    auto* response = smp_request.response;
    if (response == nullptr || response->cpu_count == 0) {
        log_message(LogLevel::Warn,
                    "SMP: Limine SMP/MP request not satisfied, continuing "
                    "single-core");
        return;
    }

    g_cpu_count = response->cpu_count;

    percpu::Cpu* bsp_cpu = percpu::find_by_lapic(response->bsp_lapic_id);
    size_t ap_slots = 0;
    for (size_t i = 0; i < response->cpu_count; ++i) {
        auto* cpu = response->cpus[i];
        if (cpu == nullptr) {
            continue;
        }
        bool is_bsp = cpu->lapic_id == response->bsp_lapic_id;
        if (is_bsp) {
            if (bsp_cpu == nullptr) {
                bsp_cpu =
                    percpu::register_cpu(cpu->lapic_id, cpu->processor_id);
            }
            continue;
        }
        percpu::Cpu* ap_cpu =
            percpu::register_cpu(cpu->lapic_id, cpu->processor_id);
        if (ap_cpu == nullptr) {
            log_message(LogLevel::Warn,
                        "SMP: ignoring AP with LAPIC ID %u (limit %zu)",
                        cpu->lapic_id,
                        static_cast<size_t>(percpu::kMaxCpus));
            continue;
        }
        scheduler::register_cpu(ap_cpu);
        cpu->extra_argument = reinterpret_cast<uint64_t>(ap_cpu);
        cpu->goto_address = smp_ap_entry;
        ++ap_slots;
    }

    if (bsp_cpu == nullptr) {
        bsp_cpu = percpu::register_cpu(response->bsp_lapic_id, 0);
    }
    percpu::set_current_cpu(bsp_cpu);
    percpu::setup_cpu_tss(*bsp_cpu);
    percpu::setup_cpu_gdt(*bsp_cpu);
    scheduler::register_cpu(bsp_cpu);

    log_message(LogLevel::Info,
                "SMP: BSP LAPIC=%u, CPUs detected: %u, booting %u AP(s)",
                response->bsp_lapic_id,
                static_cast<unsigned int>(response->cpu_count),
                static_cast<unsigned int>(ap_slots));

    if (ap_slots == 0) {
        log_message(LogLevel::Info, "SMP: no APs to boot");
    }

    log_message(LogLevel::Info,
                "SMP: scheduler sees %u CPU(s)",
                static_cast<unsigned int>(scheduler::cpu_total()));
}

size_t cpu_count() {
    return __atomic_load_n(&g_cpu_count, __ATOMIC_RELAXED);
}

size_t online_cpus() {
    return __atomic_load_n(&g_online_cpus, __ATOMIC_RELAXED);
}

}  // namespace smp
