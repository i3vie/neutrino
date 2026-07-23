#include "drivers/sensors/it87.hpp"

#include <stddef.h>
#include <stdint.h>

#include "arch/x86_64/io.hpp"
#include "drivers/log/logging.hpp"
#include "drivers/sensors/sensor.hpp"
#include "kernel/sync.hpp"

namespace it87 {
namespace {

constexpr uint16_t kConfigPorts[] = {0x2e, 0x4e};
constexpr uint8_t kLogicalDeviceEnvironmentController = 0x04;
constexpr uint8_t kTemperatureRegisters[] = {0x29, 0x2a, 0x2b};
constexpr uint8_t kVoltageRegisters[] = {
    0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28};
constexpr const char* kTemperatureNames[] = {"temp1", "temp2", "temp3"};
constexpr const char* kVoltageNames[] = {
    "in0", "in1", "in2", "in3", "in4", "in5", "in6", "in7", "in8"};

struct Adapter {
    uint16_t base;
    uint16_t chip_id;
    uint8_t voltage_lsb_mv;
    char name[32];
    sync::SpinLock io_lock;
};

Adapter g_adapter{};
bool g_initialized = false;

void config_write(uint16_t port, uint8_t reg, uint8_t value) {
    outb(port, reg);
    outb(static_cast<uint16_t>(port + 1), value);
}

uint8_t config_read(uint16_t port, uint8_t reg) {
    outb(port, reg);
    return inb(static_cast<uint16_t>(port + 1));
}

void enter_config(uint16_t port) {
    outb(port, 0x87);
    outb(port, 0x01);
    outb(port, 0x55);
    outb(port, port == 0x4e ? 0xaa : 0x55);
}

void leave_config(uint16_t port) {
    config_write(port, 0x02, 0x02);
}

bool supported_chip(uint16_t id) {
    switch (id) {
        case 0x8705:
        case 0x8712:
        case 0x8716:
        case 0x8718:
        case 0x8720:
        case 0x8721:
        case 0x8726:
        case 0x8728:
        case 0x8771:
        case 0x8772:
            return true;
        default:
            return false;
    }
}

uint8_t voltage_lsb_mv(uint16_t id) {
    switch (id) {
        case 0x8721:
        case 0x8728:
        case 0x8771:
        case 0x8772:
            return 12;
        default:
            return 16;
    }
}

uint8_t runtime_read(Adapter& adapter, uint8_t reg) {
    sync::IrqLockGuard guard(adapter.io_lock);
    outb(static_cast<uint16_t>(adapter.base + 5), reg);
    return inb(static_cast<uint16_t>(adapter.base + 6));
}

bool read_temperature(void* context, int64_t& value) {
    size_t channel = reinterpret_cast<size_t>(context);
    if (channel >= sizeof(kTemperatureRegisters)) {
        return false;
    }
    uint8_t raw = runtime_read(g_adapter, kTemperatureRegisters[channel]);
    if (raw == 0x80) {
        return false;
    }
    value = static_cast<int64_t>(static_cast<int8_t>(raw)) * 1000;
    return true;
}

bool read_voltage(void* context, int64_t& value) {
    size_t channel = reinterpret_cast<size_t>(context);
    if (channel >= sizeof(kVoltageRegisters)) {
        return false;
    }
    value = static_cast<int64_t>(runtime_read(
                g_adapter, kVoltageRegisters[channel])) *
            g_adapter.voltage_lsb_mv;
    return true;
}

char hex_digit(uint8_t nibble) {
    return nibble < 10 ? static_cast<char>('0' + nibble)
                       : static_cast<char>('a' + nibble - 10);
}

void build_adapter_name(uint16_t base) {
    const char prefix[] = "it87-isa-";
    size_t pos = 0;
    for (; prefix[pos] != '\0'; ++pos) {
        g_adapter.name[pos] = prefix[pos];
    }
    for (int shift = 12; shift >= 0; shift -= 4) {
        g_adapter.name[pos++] = hex_digit(
            static_cast<uint8_t>(base >> shift) & 0x0f);
    }
    g_adapter.name[pos] = '\0';
}

bool probe_port(uint16_t config_port) {
    uint16_t chip_id = 0;
    uint16_t base = 0;
    uint8_t active = 0;

    uint64_t flags = sync::disable_interrupts();
    enter_config(config_port);
    chip_id = static_cast<uint16_t>(config_read(config_port, 0x20)) << 8;
    chip_id |= config_read(config_port, 0x21);
    if (supported_chip(chip_id)) {
        config_write(config_port, 0x07,
                     kLogicalDeviceEnvironmentController);
        active = config_read(config_port, 0x30);
        base = static_cast<uint16_t>(config_read(config_port, 0x60)) << 8;
        base |= config_read(config_port, 0x61);
    }
    leave_config(config_port);
    sync::restore_interrupts(flags);

    if (!supported_chip(chip_id)) {
        return false;
    }
    if ((active & 0x01u) == 0 || base < 0x100 || base > 0xfff8 ||
        (base & 0x07u) != 0) {
        log_message(LogLevel::Warn,
                    "it87: chip %04x has unusable HWM base %04x (active=%u)",
                    chip_id,
                    base,
                    (active & 1u) != 0 ? 1u : 0u);
        return false;
    }

    g_adapter.base = base;
    g_adapter.chip_id = chip_id;
    g_adapter.voltage_lsb_mv = voltage_lsb_mv(chip_id);
    build_adapter_name(base);
    return true;
}

}  // namespace

void init() {
    if (g_initialized) {
        return;
    }
    g_initialized = true;

    bool found = false;
    for (uint16_t port : kConfigPorts) {
        if (probe_port(port)) {
            found = true;
            break;
        }
    }
    if (!found) {
        log_message(LogLevel::Debug, "it87: no supported ISA sensor adapter");
        return;
    }

    size_t registered = 0;
    for (size_t i = 0; i < sizeof(kTemperatureRegisters); ++i) {
        if (sensors::register_sensor(
                kTemperatureNames[i], g_adapter.name,
                descriptor_defs::SensorKind::Temperature,
                descriptor_defs::SensorUnit::MilliCelsius,
                read_temperature, reinterpret_cast<void*>(i))) {
            ++registered;
        }
    }
    for (size_t i = 0; i < sizeof(kVoltageRegisters); ++i) {
        if (sensors::register_sensor(
                kVoltageNames[i], g_adapter.name,
                descriptor_defs::SensorKind::Voltage,
                descriptor_defs::SensorUnit::Millivolt,
                read_voltage, reinterpret_cast<void*>(i))) {
            ++registered;
        }
    }

    log_message(LogLevel::Info,
                "it87: detected %04x at ISA %04x, registered %zu sensors",
                g_adapter.chip_id,
                g_adapter.base,
                registered);
}

}  // namespace it87
