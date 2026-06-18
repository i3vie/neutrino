#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../crt/syscall.hpp"
#include "../libc/include/neutrino.h"

namespace {

constexpr size_t kMaxCpuEntries = 16;
constexpr size_t kMaxTaskEntries = 256;
constexpr size_t kMaxVisibleTasks = 12;
constexpr uint32_t kTaskStateUnused = 0;
constexpr uint32_t kTaskStateReady = 1;
constexpr uint32_t kTaskStateRunning = 2;
constexpr uint32_t kTaskStateBlocked = 3;
constexpr uint32_t kTaskStateTerminated = 4;

struct TaskDelta {
    descriptor_defs::TaskUsage snapshot;
    uint64_t delta_ticks;
};

descriptor_defs::CpuUsage g_previous_cpus[kMaxCpuEntries]{};
descriptor_defs::CpuUsage g_current_cpus[kMaxCpuEntries]{};
descriptor_defs::TaskUsage g_previous_tasks[kMaxTaskEntries]{};
descriptor_defs::TaskUsage g_current_tasks[kMaxTaskEntries]{};
TaskDelta g_deltas[kMaxTaskEntries]{};
char g_render_buffer[8192]{};

void append_char(char* buffer, size_t capacity, size_t& length, char ch) {
    if (length + 1 >= capacity) {
        return;
    }
    buffer[length++] = ch;
    buffer[length] = '\0';
}

void append_text(char* buffer, size_t capacity, size_t& length, const char* text) {
    if (text == nullptr) {
        return;
    }
    while (*text != '\0' && length + 1 < capacity) {
        buffer[length++] = *text++;
    }
    buffer[length] = '\0';
}

void append_u64(char* buffer, size_t capacity, size_t& length, uint64_t value) {
    char digits[32];
    size_t count = 0;
    do {
        digits[count++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    } while (value != 0 && count < sizeof(digits));

    while (count > 0) {
        append_char(buffer, capacity, length, digits[--count]);
    }
}

void append_percent_tenths(char* buffer,
                           size_t capacity,
                           size_t& length,
                           uint64_t part,
                           uint64_t total) {
    if (total == 0) {
        append_text(buffer, capacity, length, "0.0");
        return;
    }
    uint64_t scaled = (part * 1000ull) / total;
    append_u64(buffer, capacity, length, scaled / 10ull);
    append_char(buffer, capacity, length, '.');
    append_char(buffer,
                capacity,
                length,
                static_cast<char>('0' + (scaled % 10ull)));
}

void append_percent_tenths_padded(char* buffer,
                                  size_t capacity,
                                  size_t& length,
                                  uint64_t part,
                                  uint64_t total,
                                  size_t width) {
    char percent[16];
    size_t percent_length = 0;
    percent[0] = '\0';
    append_percent_tenths(percent, sizeof(percent), percent_length, part, total);

    size_t padding = width > percent_length ? width - percent_length : 0;
    while (padding > 0) {
        append_char(buffer, capacity, length, ' ');
        --padding;
    }
    append_text(buffer, capacity, length, percent);
}

void append_padded_text(char* buffer,
                        size_t capacity,
                        size_t& length,
                        const char* text,
                        size_t width) {
    size_t written = 0;
    while (text != nullptr && text[written] != '\0' && written < width) {
        append_char(buffer, capacity, length, text[written]);
        ++written;
    }
    while (written < width) {
        append_char(buffer, capacity, length, ' ');
        ++written;
    }
}

void append_padded_u64(char* buffer,
                       size_t capacity,
                       size_t& length,
                       uint64_t value,
                       size_t width) {
    char digits[32];
    size_t count = 0;
    do {
        digits[count++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    } while (value != 0 && count < sizeof(digits));

    size_t padding = width > count ? width - count : 0;
    while (padding > 0) {
        append_char(buffer, capacity, length, ' ');
        --padding;
    }
    while (count > 0) {
        append_char(buffer, capacity, length, digits[--count]);
    }
}

const char* task_state_name(uint32_t state) {
    switch (state) {
        case kTaskStateReady:
            return "ready";
        case kTaskStateRunning:
            return "run";
        case kTaskStateBlocked:
            return "block";
        case kTaskStateTerminated:
            return "term";
        case kTaskStateUnused:
        default:
            return "unused";
    }
}

long open_monitor_descriptor(descriptor_defs::Type type) {
    return descriptor_open(static_cast<uint32_t>(type), 0, 0, 0);
}

bool clear_screen(long console) {
    if (console < 0) {
        return false;
    }
    long type = descriptor_get_type(static_cast<uint32_t>(console));
    if (type == static_cast<long>(descriptor_defs::Type::Console)) {
        return descriptor_set_property(
                   static_cast<uint32_t>(console),
                   static_cast<uint32_t>(descriptor_defs::Property::ConsoleClear),
                   nullptr,
                   0) == 0;
    }
    if (type == static_cast<long>(descriptor_defs::Type::Vty)) {
        return descriptor_set_property(
                   static_cast<uint32_t>(console),
                   static_cast<uint32_t>(descriptor_defs::Property::VtyClear),
                   nullptr,
                   0) == 0;
    }
    return false;
}

size_t read_cpu_stats(uint32_t handle,
                      descriptor_defs::CpuUsage* out,
                      size_t capacity) {
    long result = descriptor_read(handle,
                                  out,
                                  capacity * sizeof(descriptor_defs::CpuUsage),
                                  0);
    if (result <= 0) {
        return 0;
    }
    return static_cast<size_t>(result) / sizeof(descriptor_defs::CpuUsage);
}

size_t read_task_stats(uint32_t handle,
                       descriptor_defs::TaskUsage* out,
                       size_t capacity) {
    long result = descriptor_read(handle,
                                  out,
                                  capacity * sizeof(descriptor_defs::TaskUsage),
                                  0);
    if (result <= 0) {
        return 0;
    }
    return static_cast<size_t>(result) / sizeof(descriptor_defs::TaskUsage);
}

const descriptor_defs::TaskUsage* find_previous_task(
    const descriptor_defs::TaskUsage* previous,
    size_t previous_count,
    uint32_t pid) {
    for (size_t i = 0; i < previous_count; ++i) {
        if (previous[i].pid == pid) {
            return &previous[i];
        }
    }
    return nullptr;
}

size_t build_task_deltas(TaskDelta* out,
                         size_t capacity,
                         const descriptor_defs::TaskUsage* current,
                         size_t current_count,
                         const descriptor_defs::TaskUsage* previous,
                         size_t previous_count) {
    size_t count = 0;
    for (size_t i = 0; i < current_count && count < capacity; ++i) {
        if (current[i].state == kTaskStateUnused) {
            continue;
        }
        out[count].snapshot = current[i];
        const descriptor_defs::TaskUsage* prev =
            find_previous_task(previous, previous_count, current[i].pid);
        uint64_t prev_total = 0;
        if (prev != nullptr) {
            prev_total = prev->user_ticks + prev->kernel_ticks;
        }
        uint64_t total = current[i].user_ticks + current[i].kernel_ticks;
        out[count].delta_ticks = total >= prev_total ? total - prev_total : total;
        ++count;
    }
    return count;
}

void sort_task_deltas(TaskDelta* deltas, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        size_t best = i;
        for (size_t j = i + 1; j < count; ++j) {
            if (deltas[j].delta_ticks > deltas[best].delta_ticks) {
                best = j;
            }
        }
        if (best == i) {
            continue;
        }
        TaskDelta tmp = deltas[i];
        deltas[i] = deltas[best];
        deltas[best] = tmp;
    }
}

uint64_t total_cpu_delta(const descriptor_defs::CpuUsage* current,
                         const descriptor_defs::CpuUsage* previous,
                         size_t count) {
    uint64_t total = 0;
    for (size_t i = 0; i < count; ++i) {
        uint64_t current_total = current[i].user_ticks + current[i].kernel_ticks +
                                 current[i].idle_ticks + current[i].irq_ticks;
        uint64_t previous_total = previous[i].user_ticks + previous[i].kernel_ticks +
                                  previous[i].idle_ticks + previous[i].irq_ticks;
        total += current_total >= previous_total ? current_total - previous_total
                                                 : current_total;
    }
    return total;
}

void wait_for_next_second() {
    NeutrinoWallTime now{};
    NeutrinoWallTime later{};
    if (!neutrino_get_time(&now)) {
        sleep_seconds(1);
        return;
    }
    for (;;) {
        sleep_ms(1);
        if (!neutrino_get_time(&later)) {
            return;
        }
        if (later.unix_seconds != now.unix_seconds) {
            return;
        }
    }
}

void append_cpu_line(char* buffer,
                     size_t capacity,
                     size_t& length,
                     const descriptor_defs::CpuUsage& current,
                     const descriptor_defs::CpuUsage& previous) {
    uint64_t user = current.user_ticks >= previous.user_ticks
                        ? current.user_ticks - previous.user_ticks
                        : current.user_ticks;
    uint64_t kernel = current.kernel_ticks >= previous.kernel_ticks
                          ? current.kernel_ticks - previous.kernel_ticks
                          : current.kernel_ticks;
    uint64_t idle = current.idle_ticks >= previous.idle_ticks
                        ? current.idle_ticks - previous.idle_ticks
                        : current.idle_ticks;
    uint64_t irq = current.irq_ticks >= previous.irq_ticks
                       ? current.irq_ticks - previous.irq_ticks
                       : current.irq_ticks;
    uint64_t total = user + kernel + idle + irq;

    append_text(buffer, capacity, length, "cpu");
    append_u64(buffer, capacity, length, current.cpu_index);
    append_text(buffer, capacity, length, "  user ");
    append_percent_tenths_padded(buffer, capacity, length, user, total, 5);
    append_text(buffer, capacity, length, "%  kernel ");
    append_percent_tenths_padded(buffer, capacity, length, kernel, total, 5);
    append_text(buffer, capacity, length, "%  idle ");
    append_percent_tenths_padded(buffer, capacity, length, idle, total, 5);
    append_text(buffer, capacity, length, "%  irq ");
    append_percent_tenths_padded(buffer, capacity, length, irq, total, 5);
    append_text(buffer, capacity, length, "%\n");
}

void append_task_table(char* buffer,
                       size_t capacity,
                       size_t& length,
                       TaskDelta* deltas,
                       size_t count,
                       uint64_t interval_total) {
    append_text(buffer, capacity, length, "\n");
    append_text(buffer, capacity, length, "       pid   cpu%  state  kind    image\n");
    append_text(buffer, capacity, length, "------------------------------------------------------------\n");

    size_t visible = count < kMaxVisibleTasks ? count : kMaxVisibleTasks;
    for (size_t i = 0; i < visible; ++i) {
        append_padded_u64(buffer, capacity, length, deltas[i].snapshot.pid, 10);
        append_text(buffer, capacity, length, "  ");
        append_percent_tenths_padded(buffer,
                                     capacity,
                                     length,
                                     deltas[i].delta_ticks,
                                     interval_total,
                                     5);
        append_text(buffer, capacity, length, "  ");
        append_padded_text(buffer,
                           capacity,
                           length,
                           task_state_name(deltas[i].snapshot.state),
                           5);
        append_text(buffer, capacity, length, "  ");
        append_padded_text(buffer,
                           capacity,
                           length,
                           (deltas[i].snapshot.flags &
                            descriptor_defs::kTaskStatFlagKernel)
                               ? "kernel"
                               : "user",
                           6);
        append_text(buffer, capacity, length, "  ");
        append_text(buffer, capacity, length, deltas[i].snapshot.image_path);
        append_text(buffer, capacity, length, "\n");
    }
}

}  // namespace

extern "C" int main(uint64_t, uint64_t) {
    long console = neutrino_open_stdout();
    if (console < 0) {
        return 1;
    }

    long cpu_desc = open_monitor_descriptor(descriptor_defs::Type::CpuStats);
    if (cpu_desc < 0) {
        neutrino_write_line(console, "top: failed to open cpu stats (missing monitor capability?)");
        return 1;
    }

    long task_desc = open_monitor_descriptor(descriptor_defs::Type::TaskStats);
    if (task_desc < 0) {
        neutrino_write_line(console, "top: failed to open task stats (missing monitor capability?)");
        descriptor_close(static_cast<uint32_t>(cpu_desc));
        return 1;
    }

    size_t previous_cpu_count =
        read_cpu_stats(static_cast<uint32_t>(cpu_desc), g_previous_cpus, kMaxCpuEntries);
    size_t previous_task_count =
        read_task_stats(static_cast<uint32_t>(task_desc), g_previous_tasks, kMaxTaskEntries);

    for (;;) {
        wait_for_next_second();

        size_t current_cpu_count =
            read_cpu_stats(static_cast<uint32_t>(cpu_desc), g_current_cpus, kMaxCpuEntries);
        size_t current_task_count =
            read_task_stats(static_cast<uint32_t>(task_desc), g_current_tasks, kMaxTaskEntries);
        size_t delta_count = build_task_deltas(g_deltas,
                                               kMaxTaskEntries,
                                               g_current_tasks,
                                               current_task_count,
                                               g_previous_tasks,
                                               previous_task_count);
        sort_task_deltas(g_deltas, delta_count);

        char* buffer = g_render_buffer;
        size_t length = 0;
        buffer[0] = '\0';

        clear_screen(console);
        append_text(buffer, sizeof(g_render_buffer), length, "neutrino top\n");
        append_text(buffer, sizeof(g_render_buffer), length, "\n");

        size_t cpu_count = current_cpu_count < previous_cpu_count
                               ? current_cpu_count
                               : previous_cpu_count;
        for (size_t i = 0; i < cpu_count; ++i) {
            append_cpu_line(buffer,
                            sizeof(g_render_buffer),
                            length,
                            g_current_cpus[i],
                            g_previous_cpus[i]);
        }

        uint64_t interval_total =
            total_cpu_delta(g_current_cpus, g_previous_cpus, cpu_count);
        append_task_table(buffer,
                          sizeof(g_render_buffer),
                          length,
                          g_deltas,
                          delta_count,
                          interval_total == 0 ? 1 : interval_total);

        neutrino_write(console, buffer);

        memcpy(g_previous_cpus, g_current_cpus, sizeof(g_previous_cpus));
        memcpy(g_previous_tasks, g_current_tasks, sizeof(g_previous_tasks));
        previous_cpu_count = current_cpu_count;
        previous_task_count = current_task_count;
    }
}
