#include "serial.hpp"

#include <stddef.h>
#include <stdint.h>

namespace {

constexpr uint16_t COM1_PORT = 0x3F8;

inline void outb(uint16_t port, uint8_t value) {
    asm volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

inline uint8_t inb(uint16_t port) {
    uint8_t value;
    asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

bool g_initialized = false;

}  // namespace

namespace serial {

void init() {
    if (g_initialized) {
        return;
    }

    outb(COM1_PORT + 1, 0x00);  // disable interrupts
    outb(COM1_PORT + 3, 0x80);  // enable DLAB
    outb(COM1_PORT + 0, 0x03);  // divisor low (38400 baud)
    outb(COM1_PORT + 1, 0x00);  // divisor high
    outb(COM1_PORT + 3, 0x03);  // 8 bits, no parity, one stop
    outb(COM1_PORT + 2, 0xC7);  // enable FIFO, clear, 14-byte threshold
    outb(COM1_PORT + 4, 0x0B);  // IRQs enabled, RTS/DSR set

    g_initialized = true;
}

void write_char(char c) {
    if (!g_initialized) {
        init();
    }

    if (c == '\n') {
        write_char('\r');
    }

    while ((inb(COM1_PORT + 5) & 0x20) == 0) {
        asm volatile("pause");
    }
    outb(COM1_PORT, static_cast<uint8_t>(c));
}

void write(const char* data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        write_char(data[i]);
    }
}

void write_string(const char* str) {
    if (str == nullptr) {
        return;
    }
    while (*str) {
        write_char(*str++);
    }
}

size_t read(char* buffer, size_t len) {
    if (buffer == nullptr || len == 0) {
        return 0;
    }
    size_t read_count = 0;
    while (read_count < len) {
        if ((inb(COM1_PORT + 5) & 0x01) == 0) {
            break;
        }
        buffer[read_count++] = static_cast<char>(inb(COM1_PORT));
    }
    return read_count;
}

bool data_available() {
    return (inb(COM1_PORT + 5) & 0x01) != 0;
}

}  // namespace serial
