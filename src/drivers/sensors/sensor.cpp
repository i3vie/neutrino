#include "drivers/sensors/sensor.hpp"

#include "kernel/sync.hpp"

namespace sensors {
namespace {

constexpr size_t kMaxSensors = 64;

struct Sensor {
    descriptor_defs::SensorInfo info;
    ReadFn read;
    void* context;
};

Sensor g_sensors[kMaxSensors]{};
size_t g_sensor_count = 0;
sync::SpinLock g_lock;

void copy_name(char* destination, size_t capacity, const char* source) {
    size_t i = 0;
    if (source != nullptr) {
        while (i + 1 < capacity && source[i] != '\0') {
            destination[i] = source[i];
            ++i;
        }
    }
    destination[i] = '\0';
}

}  // namespace

bool register_sensor(const char* name,
                     const char* adapter,
                     descriptor_defs::SensorKind kind,
                     descriptor_defs::SensorUnit unit,
                     ReadFn read_fn,
                     void* context) {
    if (name == nullptr || adapter == nullptr || read_fn == nullptr) {
        return false;
    }

    sync::IrqLockGuard guard(g_lock);
    if (g_sensor_count >= kMaxSensors) {
        return false;
    }

    Sensor& sensor = g_sensors[g_sensor_count];
    copy_name(sensor.info.name, sizeof(sensor.info.name), name);
    copy_name(sensor.info.adapter, sizeof(sensor.info.adapter), adapter);
    sensor.info.kind = kind;
    sensor.info.unit = unit;
    sensor.info.index = static_cast<uint32_t>(g_sensor_count);
    sensor.read = read_fn;
    sensor.context = context;
    ++g_sensor_count;
    return true;
}

size_t count() {
    sync::IrqLockGuard guard(g_lock);
    return g_sensor_count;
}

bool info(size_t index, descriptor_defs::SensorInfo& out) {
    sync::IrqLockGuard guard(g_lock);
    if (index >= g_sensor_count) {
        return false;
    }
    out = g_sensors[index].info;
    return true;
}

bool read(size_t index, descriptor_defs::SensorSample& out) {
    ReadFn read_fn = nullptr;
    void* context = nullptr;
    {
        sync::IrqLockGuard guard(g_lock);
        if (index >= g_sensor_count) {
            return false;
        }
        read_fn = g_sensors[index].read;
        context = g_sensors[index].context;
    }

    int64_t value = 0;
    bool valid = read_fn(context, value);
    out.value = value;
    out.flags = valid
        ? static_cast<uint32_t>(descriptor_defs::kSensorSampleValid)
        : 0u;
    out.reserved = 0;
    return true;
}

}  // namespace sensors
