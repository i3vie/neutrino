#include "mouse.hpp"

#include "arch/x86_64/io.hpp"
#include "../interrupts/pic.hpp"
#include "../log/logging.hpp"
#include "../../kernel/descriptor.hpp"

namespace mouse {
namespace {

constexpr size_t kBufferSize = 64;
constexpr size_t kInputSlots = 6;

constexpr uint16_t kDataPort = 0x60;
constexpr uint16_t kStatusPort = 0x64;

constexpr uint8_t kStatusOutputFull = 1u << 0;
constexpr uint8_t kStatusInputFull = 1u << 1;
constexpr uint8_t kStatusAuxData = 1u << 5;

constexpr uint8_t kCommandEnableAux = 0xA8;
constexpr uint8_t kCommandReadConfig = 0x20;
constexpr uint8_t kCommandWriteConfig = 0x60;
constexpr uint8_t kCommandWriteAux = 0xD4;

constexpr uint8_t kMouseSetDefaults = 0xF6;
constexpr uint8_t kMouseEnableStream = 0xF4;
constexpr uint8_t kMouseAck = 0xFA;

struct SlotBuffer {
    Event data[kBufferSize];
    size_t head;
    size_t tail;
};

SlotBuffer g_buffers[kInputSlots];
bool g_initialized = false;
uint8_t g_packet[3];
uint8_t g_packet_index = 0;

bool wait_input_clear() {
    for (size_t i = 0; i < 100000; ++i) {
        if ((inb(kStatusPort) & kStatusInputFull) == 0) {
            return true;
        }
    }
    return false;
}

bool wait_output_full() {
    for (size_t i = 0; i < 100000; ++i) {
        if ((inb(kStatusPort) & kStatusOutputFull) != 0) {
            return true;
        }
    }
    return false;
}

void write_command(uint8_t cmd) {
    if (!wait_input_clear()) {
        return;
    }
    outb(kStatusPort, cmd);
}

void write_data(uint8_t data) {
    if (!wait_input_clear()) {
        return;
    }
    outb(kDataPort, data);
}

uint8_t read_data() {
    if (!wait_output_full()) {
        return 0;
    }
    return inb(kDataPort);
}

bool write_mouse(uint8_t data) {
    write_command(kCommandWriteAux);
    write_data(data);
    uint8_t ack = read_data();
    return ack == kMouseAck;
}

void enqueue(uint32_t slot, const Event& ev) {
    if (slot >= kInputSlots) {
        return;
    }
    SlotBuffer& buf = g_buffers[slot];
    size_t next = (buf.head + 1) % kBufferSize;
    if (next == buf.tail) {
        return;
    }
    buf.data[buf.head] = ev;
    buf.head = next;
}

bool dequeue(uint32_t slot, Event& ev) {
    if (slot >= kInputSlots) {
        return false;
    }
    SlotBuffer& buf = g_buffers[slot];
    if (buf.head == buf.tail) {
        return false;
    }
    ev = buf.data[buf.tail];
    buf.tail = (buf.tail + 1) % kBufferSize;
    return true;
}

}  // namespace

void init() {
    if (g_initialized) {
        return;
    }
    for (auto& buf : g_buffers) {
        buf.head = 0;
        buf.tail = 0;
    }
    g_packet_index = 0;

    write_command(kCommandEnableAux);
    write_command(kCommandReadConfig);
    uint8_t config = read_data();
    config |= (1u << 1);   // enable IRQ12
    config &= ~(1u << 5);  // enable mouse clock
    write_command(kCommandWriteConfig);
    write_data(config);

    if (!write_mouse(kMouseSetDefaults)) {
        log_message(LogLevel::Warn, "Mouse: failed to set defaults");
    }
    if (!write_mouse(kMouseEnableStream)) {
        log_message(LogLevel::Warn, "Mouse: failed to enable streaming");
    }

    pic::set_mask(2, false);
    pic::set_mask(12, false);

    g_initialized = true;
}

void handle_irq() {
    uint8_t status = inb(kStatusPort);
    if ((status & kStatusOutputFull) == 0) {
        return;
    }
    if ((status & kStatusAuxData) == 0) {
        return;
    }

    uint8_t data = inb(kDataPort);
    if (g_packet_index == 0 && (data & 0x08) == 0) {
        return;
    }
    g_packet[g_packet_index++] = data;
    if (g_packet_index < 3) {
        return;
    }
    g_packet_index = 0;

    Event ev{};
    ev.buttons = static_cast<uint8_t>(g_packet[0] & 0x07);
    ev.dx = static_cast<int8_t>(g_packet[1]);
    ev.dy = static_cast<int8_t>(g_packet[2]);
    ev.reserved = 0;

    uint32_t slot = descriptor::framebuffer_active_slot();
    if (slot >= kInputSlots) {
        slot = 0;
    }
    enqueue(slot, ev);
}

size_t read(uint32_t slot, Event* buffer, size_t max_events) {
    if (buffer == nullptr || max_events == 0) {
        return 0;
    }
    if (slot >= kInputSlots) {
        return 0;
    }

    size_t count = 0;
    while (count < max_events) {
        Event ev{};
        if (!dequeue(slot, ev)) {
            break;
        }
        buffer[count++] = ev;
    }
    return count;
}

}  // namespace mouse
