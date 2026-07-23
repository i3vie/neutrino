#include "drivers/sensors/acpi_thermal.hpp"

#include <stddef.h>
#include <stdint.h>

#include "drivers/log/logging.hpp"
#include "drivers/sensors/sensor.hpp"

extern "C" {
#include <uacpi/namespace.h>
#include <uacpi/uacpi.h>
#include <uacpi/utilities.h>
}

namespace acpi_thermal {
namespace {

constexpr size_t kMaxThermalZones = 16;
constexpr uint32_t kStaPresent = 1u << 0;
constexpr uint32_t kStaFunctioning = 1u << 3;

struct ThermalZone {
    uacpi_namespace_node* node;
    char adapter[32];
};

ThermalZone g_zones[kMaxThermalZones]{};
size_t g_zone_count = 0;
bool g_initialized = false;
volatile bool g_evaluating = false;

char safe_name_char(char ch) {
    bool alpha = (ch >= 'A' && ch <= 'Z') ||
                 (ch >= 'a' && ch <= 'z');
    bool numeric = ch >= '0' && ch <= '9';
    return (alpha || numeric || ch == '_') ? ch : '_';
}

void build_adapter_name(ThermalZone& zone,
                        uacpi_namespace_node* node,
                        size_t index) {
    constexpr char prefix[] = "acpi-thermal-";
    size_t pos = 0;
    while (prefix[pos] != '\0') {
        zone.adapter[pos] = prefix[pos];
        ++pos;
    }

    uacpi_object_name name = uacpi_namespace_node_name(node);
    for (size_t i = 0; i < sizeof(name.text); ++i) {
        zone.adapter[pos++] = safe_name_char(name.text[i]);
    }
    zone.adapter[pos++] = '-';

    char digits[20];
    size_t digit_count = 0;
    do {
        digits[digit_count++] = static_cast<char>('0' + index % 10);
        index /= 10;
    } while (index != 0 && digit_count < sizeof(digits));
    while (digit_count != 0) {
        zone.adapter[pos++] = digits[--digit_count];
    }
    zone.adapter[pos] = '\0';
}

bool read_temperature(void* context, int64_t& value) {
    auto* zone = static_cast<ThermalZone*>(context);
    if (zone == nullptr || zone->node == nullptr) {
        return false;
    }

    // The current kernel uACPI glue does not provide distinct thread IDs.
    // Reject an overlapping sample instead of allowing concurrent AML method
    // evaluation to appear recursive to the interpreter.
    if (__atomic_test_and_set(&g_evaluating, __ATOMIC_ACQUIRE)) {
        return false;
    }

    uacpi_u64 tenths_kelvin = 0;
    uacpi_status status = uacpi_eval_simple_integer(
        zone->node, "_TMP", &tenths_kelvin);
    __atomic_clear(&g_evaluating, __ATOMIC_RELEASE);
    if (status != UACPI_STATUS_OK) {
        return false;
    }

    constexpr uint64_t kMaximumConvertible =
        (static_cast<uint64_t>(INT64_MAX) + 273150ull) / 100ull;
    if (tenths_kelvin > kMaximumConvertible) {
        return false;
    }

    // ACPI temperatures are expressed in tenths of a Kelvin.
    value = static_cast<int64_t>(tenths_kelvin * 100ull) - 273150;
    return true;
}

uacpi_iteration_decision visit_zone(void*,
                                    uacpi_namespace_node* node,
                                    uacpi_u32) {
    if (node == nullptr || g_zone_count >= kMaxThermalZones) {
        return g_zone_count >= kMaxThermalZones
            ? UACPI_ITERATION_DECISION_BREAK
            : UACPI_ITERATION_DECISION_CONTINUE;
    }

    uacpi_u32 sta = 0;
    if (uacpi_eval_sta(node, &sta) != UACPI_STATUS_OK ||
        (sta & (kStaPresent | kStaFunctioning)) !=
            (kStaPresent | kStaFunctioning)) {
        return UACPI_ITERATION_DECISION_CONTINUE;
    }

    ThermalZone& zone = g_zones[g_zone_count];
    zone.node = node;
    build_adapter_name(zone, node, g_zone_count);

    int64_t initial_value = 0;
    if (!read_temperature(&zone, initial_value)) {
        zone = ThermalZone{};
        return UACPI_ITERATION_DECISION_CONTINUE;
    }

    if (!sensors::register_sensor(
            "temp1", zone.adapter,
            descriptor_defs::SensorKind::Temperature,
            descriptor_defs::SensorUnit::MilliCelsius,
            read_temperature, &zone)) {
        zone = ThermalZone{};
        return UACPI_ITERATION_DECISION_BREAK;
    }

    const uacpi_char* path =
        uacpi_namespace_node_generate_absolute_path(node);
    log_message(LogLevel::Info,
                "ACPI thermal: registered %s as %s (initial=%lld mC)",
                path != nullptr ? path : "<unknown>",
                zone.adapter,
                static_cast<long long>(initial_value));
    if (path != nullptr) {
        uacpi_free_absolute_path(path);
    }
    ++g_zone_count;
    return UACPI_ITERATION_DECISION_CONTINUE;
}

}  // namespace

void init() {
    if (g_initialized) {
        return;
    }
    g_initialized = true;

    if (uacpi_get_current_init_level() <
        UACPI_INIT_LEVEL_NAMESPACE_INITIALIZED) {
        log_message(LogLevel::Debug,
                    "ACPI thermal: namespace is not fully initialized");
        return;
    }

    uacpi_status status = uacpi_namespace_for_each_child(
        uacpi_namespace_root(), visit_zone, nullptr,
        UACPI_OBJECT_THERMAL_ZONE_BIT, UACPI_MAX_DEPTH_ANY, nullptr);
    if (status != UACPI_STATUS_OK) {
        log_message(LogLevel::Warn,
                    "ACPI thermal: namespace scan failed: %s",
                    uacpi_status_to_string(status));
        return;
    }

    log_message(LogLevel::Info,
                "ACPI thermal: registered %zu thermal zone(s)",
                g_zone_count);
}

}  // namespace acpi_thermal
