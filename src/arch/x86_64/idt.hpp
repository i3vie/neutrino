#pragma once
#include <stdint.h>
extern "C" void idt_install();
extern "C" void isr_page_fault_stub();
extern "C" void isr_default_stub();
