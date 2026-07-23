#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "descriptors.hpp"
#include "../crt/syscall.hpp"

namespace {

constexpr uint32_t kDescConsole =
    static_cast<uint32_t>(descriptor_defs::Type::Console);
constexpr uint32_t kDescSensor =
    static_cast<uint32_t>(descriptor_defs::Type::Sensor);

void print(long console, const char* text) {
    if (console >= 0 && text != nullptr) {
        descriptor_write(static_cast<uint32_t>(console), text, strlen(text));
    }
}

void print_u64(long console, uint64_t value) {
    char buffer[24];
    size_t pos = sizeof(buffer);
    buffer[--pos] = '\0';
    do {
        buffer[--pos] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0 && pos != 0);
    print(console, buffer + pos);
}

void print_fixed_thousandths(long console, int64_t value, const char* suffix) {
    if (value < 0) {
        print(console, "-");
    } else {
        print(console, "+");
    }
    uint64_t magnitude = value < 0
        ? static_cast<uint64_t>(-(value + 1)) + 1
        : static_cast<uint64_t>(value);
    print_u64(console, magnitude / 1000);
    print(console, ".");
    uint64_t fraction = magnitude % 1000;
    if (fraction < 100) print(console, "0");
    if (fraction < 10) print(console, "0");
    print_u64(console, fraction);
    print(console, suffix);
}

bool starts_with(const char* text, const char* prefix) {
    if (text == nullptr || prefix == nullptr) {
        return false;
    }
    while (*prefix != '\0') {
        if (*text++ != *prefix++) {
            return false;
        }
    }
    return true;
}

const char* adapter_type(const char* adapter) {
    if (starts_with(adapter, "acpi-thermal-")) {
        return "ACPI thermal zone";
    }
    if (starts_with(adapter, "it87-isa-")) {
        return "ISA adapter";
    }
    return "Platform sensor";
}

void print_sample(long console,
                  const descriptor_defs::SensorInfo& info,
                  const descriptor_defs::SensorSample& sample) {
    print(console, "  ");
    print(console, info.name);
    print(console, ": ");
    if ((sample.flags & descriptor_defs::kSensorSampleValid) == 0) {
        print(console, "N/A\n");
        return;
    }

    switch (info.unit) {
        case descriptor_defs::SensorUnit::MilliCelsius:
            print_fixed_thousandths(console, sample.value, " C\n");
            break;
        case descriptor_defs::SensorUnit::Millivolt:
            print_fixed_thousandths(console, sample.value, " V\n");
            break;
        case descriptor_defs::SensorUnit::Rpm:
            if (sample.value < 0) {
                print(console, "N/A\n");
            } else {
                print_u64(console, static_cast<uint64_t>(sample.value));
                print(console, " RPM\n");
            }
            break;
    }
}

}  // namespace

int main(uint64_t arg_ptr, uint64_t) {
    long console = process_get_standard_descriptor(1);
    if (console < 0) {
        console = descriptor_open(kDescConsole, 0);
    }
    const char* args = reinterpret_cast<const char*>(arg_ptr);
    if (args != nullptr) {
        while (*args == ' ' || *args == '\t' || *args == '\r' || *args == '\n') {
            ++args;
        }
        if (*args != '\0') {
            print(console, "usage: sensors\n");
            return 1;
        }
    }

    bool any = false;
    char previous_adapter[32]{};
    for (uint64_t index = 0;; ++index) {
        long handle = descriptor_open(kDescSensor, index, 0, 0);
        if (handle < 0) {
            break;
        }

        descriptor_defs::SensorInfo info{};
        descriptor_defs::SensorSample sample{};
        long info_result = descriptor_get_property(
            static_cast<uint32_t>(handle),
            static_cast<uint32_t>(descriptor_defs::Property::SensorInfo),
            &info, sizeof(info));
        long bytes = descriptor_read(static_cast<uint32_t>(handle),
                                     &sample, sizeof(sample), 0);
        descriptor_close(static_cast<uint32_t>(handle));
        if (info_result != 0 || bytes != static_cast<long>(sizeof(sample))) {
            continue;
        }

        if (!any || strcmp(previous_adapter, info.adapter) != 0) {
            if (any) print(console, "\n");
            print(console, info.adapter);
            print(console, "\nAdapter: ");
            print(console, adapter_type(info.adapter));
            print(console, "\n");
            size_t i = 0;
            while (i + 1 < sizeof(previous_adapter) && info.adapter[i] != '\0') {
                previous_adapter[i] = info.adapter[i];
                ++i;
            }
            previous_adapter[i] = '\0';
        }
        print_sample(console, info, sample);
        any = true;
    }

    if (!any) {
        print(console, "No supported hardware sensors found.\n");
        return 1;
    }
    return 0;
}
