#include "pic.hpp"

#include "arch/x86_64/io.hpp"

namespace {

constexpr uint16_t PIC1_COMMAND = 0x20;
constexpr uint16_t PIC1_DATA    = 0x21;
constexpr uint16_t PIC2_COMMAND = 0xA0;
constexpr uint16_t PIC2_DATA    = 0xA1;

constexpr uint8_t PIC_EOI = 0x20;

uint8_t g_master_mask = 0xFF;
uint8_t g_slave_mask  = 0xFF;

void apply_masks() {
    outb(PIC1_DATA, g_master_mask);
    outb(PIC2_DATA, g_slave_mask);
}

}  // namespace

namespace pic {

void init() {
    g_master_mask = inb(PIC1_DATA);
    g_slave_mask = inb(PIC2_DATA);

    outb(PIC1_COMMAND, 0x11);
    io_wait();
    outb(PIC2_COMMAND, 0x11);
    io_wait();

    outb(PIC1_DATA, 0x20);
    io_wait();
    outb(PIC2_DATA, 0x28);
    io_wait();

    outb(PIC1_DATA, 0x04);
    io_wait();
    outb(PIC2_DATA, 0x02);
    io_wait();

    outb(PIC1_DATA, 0x01);
    io_wait();
    outb(PIC2_DATA, 0x01);
    io_wait();

    g_master_mask = 0xFE;  // unmask IRQ0 only
    g_slave_mask = 0xFF;
    apply_masks();
}

void send_eoi(uint8_t irq) {
    if (irq >= 8) {
        outb(PIC2_COMMAND, PIC_EOI);
    }
    outb(PIC1_COMMAND, PIC_EOI);
}

void set_mask(uint8_t irq, bool masked) {
    if (irq < 8) {
        if (masked) {
            g_master_mask |= (1u << irq);
        } else {
            g_master_mask &= ~(1u << irq);
        }
    } else {
        uint8_t line = static_cast<uint8_t>(irq - 8);
        if (masked) {
            g_slave_mask |= (1u << line);
        } else {
            g_slave_mask &= ~(1u << line);
        }
    }
    apply_masks();
}

}  // namespace pic

