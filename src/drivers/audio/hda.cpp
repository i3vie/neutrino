#include "drivers/audio/hda.hpp"

#include "arch/x86_64/memory/paging.hpp"
#include "drivers/driver_registry.hpp"
#include "drivers/log/logging.hpp"
#include "drivers/pci/pci.hpp"
#include "kernel/memory/physical_allocator.hpp"
#include "lib/mem.hpp"

namespace hda {
namespace {

constexpr size_t kPageSize = 4096;
constexpr size_t kDmaPages = 16;
constexpr size_t kDmaBytes = kDmaPages * kPageSize;
constexpr uint64_t kMmioVirtBase = 0xFFFFE50000000000ull;
constexpr uint64_t kMmioLength = 0x4000;
constexpr uint16_t kPcmFormat = 0x0011;  // 48 kHz, 16-bit, two channels.

constexpr uint32_t kGctlReset = 1u << 0;
constexpr uint32_t kStreamCtlRun = 1u << 1;
constexpr uint32_t kStreamCtlReset = 1u << 0;
constexpr uint8_t kStreamStsBcis = 1u << 2;
constexpr uint32_t kWidgetTypeOutput = 0;
constexpr uint32_t kWidgetTypePin = 4;

constexpr driver_registry::PciMatch kPciMatches[] = {
    {driver_registry::kAnyVendor, driver_registry::kAnyDevice, 0x04, 0x03,
     driver_registry::kAnyProgIf},
};

struct [[gnu::packed]] BdlEntry {
    uint32_t address_low;
    uint32_t address_high;
    uint32_t length;
    uint32_t flags;
};

struct Widget {
    bool present;
    uint32_t caps;
    uint32_t pin_caps;
    uint32_t default_config;
    uint8_t connections[32];
    uint8_t connection_count;
};

struct State {
    bool initialized;
    bool active;
    pci::PciDevice device;
    volatile uint8_t* regs;
    uint8_t codec;
    uint8_t function_group;
    uint8_t dac;
    uint8_t pin;
    uint8_t stream_index;
    uint8_t stream_tag;
    uint64_t dma_phys;
    uint8_t* dma;
    uint64_t bdl_phys;
    BdlEntry* bdl;
    Widget widgets[256];
    volatile int lock;
};

State g_state{};

inline uint8_t read8(size_t offset) {
    return *reinterpret_cast<volatile uint8_t*>(g_state.regs + offset);
}
inline uint16_t read16(size_t offset) {
    return *reinterpret_cast<volatile uint16_t*>(g_state.regs + offset);
}
inline uint32_t read32(size_t offset) {
    return *reinterpret_cast<volatile uint32_t*>(g_state.regs + offset);
}
inline void write8(size_t offset, uint8_t value) {
    *reinterpret_cast<volatile uint8_t*>(g_state.regs + offset) = value;
}
inline void write16(size_t offset, uint16_t value) {
    *reinterpret_cast<volatile uint16_t*>(g_state.regs + offset) = value;
}
inline void write32(size_t offset, uint32_t value) {
    *reinterpret_cast<volatile uint32_t*>(g_state.regs + offset) = value;
}

void relax() { asm volatile("pause"); }

bool wait32(size_t offset, uint32_t mask, uint32_t wanted,
            uint32_t spins = 2000000) {
    while (spins-- != 0) {
        if ((read32(offset) & mask) == wanted) return true;
        relax();
    }
    return false;
}

uint64_t bar0(const pci::PciDevice& device) {
    uint32_t low = pci::read_config32(device, 0x10);
    if ((low & 1u) != 0) return 0;
    uint64_t base = low & ~0xfull;
    if (((low >> 1) & 3u) == 2u)
        base |= static_cast<uint64_t>(pci::read_config32(device, 0x14)) << 32;
    return base;
}

volatile uint8_t* map_mmio(uint64_t phys) {
    uint64_t first = phys & ~(static_cast<uint64_t>(kPageSize) - 1);
    size_t offset = static_cast<size_t>(phys - first);
    size_t pages = (offset + kMmioLength + kPageSize - 1) / kPageSize;
    uint64_t flags = PAGE_FLAG_WRITE | PAGE_FLAG_WRITE_THROUGH |
                     PAGE_FLAG_CACHE_DISABLE;
    for (size_t i = 0; i < pages; ++i) {
        if (!paging_map_page(kMmioVirtBase + i * kPageSize,
                             first + i * kPageSize, flags))
            return nullptr;
    }
    return reinterpret_cast<volatile uint8_t*>(kMmioVirtBase + offset);
}

// Uses the controller's immediate command interface. This keeps discovery
// polled and compact; only PCM samples require a DMA ring.
bool verb(uint8_t codec, uint8_t node, uint32_t payload, uint32_t& response) {
    constexpr size_t kIcoi = 0x60;
    constexpr size_t kIcii = 0x64;
    constexpr size_t kIcis = 0x68;
    for (uint32_t i = 0; i < 1000000 && (read16(kIcis) & 1u); ++i) relax();
    if (read16(kIcis) & 1u) return false;
    write16(kIcis, 2u);  // Clear stale response-valid.
    write32(kIcoi, (static_cast<uint32_t>(codec) << 28) |
                       (static_cast<uint32_t>(node) << 20) | payload);
    write16(kIcis, 1u);
    for (uint32_t i = 0; i < 1000000; ++i) {
        uint16_t status = read16(kIcis);
        if ((status & 1u) == 0 && (status & 2u) != 0) {
            response = read32(kIcii);
            write16(kIcis, 2u);
            return true;
        }
        relax();
    }
    return false;
}

uint32_t get_parameter(uint8_t node, uint8_t parameter) {
    uint32_t result = 0;
    (void)verb(g_state.codec, node, 0xF0000u | parameter, result);
    return result;
}

bool set_verb(uint8_t node, uint32_t payload) {
    uint32_t ignored = 0;
    return verb(g_state.codec, node, payload, ignored);
}

void read_connections(uint8_t node, Widget& widget) {
    uint32_t length = get_parameter(node, 0x0E);
    bool long_form = (length & 0x80u) != 0;
    uint8_t count = static_cast<uint8_t>(length & 0x7Fu);
    uint8_t stride = long_form ? 2 : 4;
    uint16_t previous = 0;
    for (uint8_t index = 0; index < count && widget.connection_count < 32;
         index = static_cast<uint8_t>(index + stride)) {
        uint32_t packed = 0;
        if (!verb(g_state.codec, node, 0xF0200u | index, packed)) break;
        for (uint8_t part = 0; part < stride && index + part < count; ++part) {
            uint16_t raw = long_form ? static_cast<uint16_t>(packed >> (part * 16))
                                     : static_cast<uint8_t>(packed >> (part * 8));
            bool range = long_form ? (raw & 0x8000u) : (raw & 0x80u);
            uint16_t nid = raw & (long_form ? 0x7FFFu : 0x7Fu);
            if (range && previous < nid) {
                for (uint16_t n = previous + 1;
                     n <= nid && widget.connection_count < 32; ++n)
                    widget.connections[widget.connection_count++] =
                        static_cast<uint8_t>(n);
            } else if (nid != 0) {
                widget.connections[widget.connection_count++] =
                    static_cast<uint8_t>(nid);
            }
            previous = nid;
        }
    }
}

bool find_dac_path(uint8_t node, bool visited[256]) {
    if (visited[node] || !g_state.widgets[node].present) return false;
    visited[node] = true;
    Widget& widget = g_state.widgets[node];
    if (((widget.caps >> 20) & 0xFu) == kWidgetTypeOutput) {
        g_state.dac = node;
        return true;
    }
    for (uint8_t i = 0; i < widget.connection_count; ++i) {
        if (find_dac_path(widget.connections[i], visited)) {
            // Selectors and mixers both accept this; harmless when ignored.
            (void)set_verb(node, 0x70100u | i);
            return true;
        }
    }
    return false;
}

int pin_score(const Widget& widget) {
    if ((widget.pin_caps & (1u << 4)) == 0) return -1;
    if (((widget.default_config >> 30) & 3u) == 1u) return -1;
    uint8_t device = static_cast<uint8_t>((widget.default_config >> 20) & 0xFu);
    if (device == 1) return 30;  // Speaker
    if (device == 2) return 20;  // Headphone
    if (device == 0) return 10;  // Line out
    return 0;
}

bool enumerate_codec(uint8_t codec) {
    g_state.codec = codec;
    uint32_t vendor = get_parameter(0, 0x00);
    uint32_t root_nodes = get_parameter(0, 0x04);
    uint8_t root_start = static_cast<uint8_t>(root_nodes >> 16);
    uint8_t root_count = static_cast<uint8_t>(root_nodes);
    for (uint16_t n = root_start; n < root_start + root_count; ++n) {
        uint32_t type = get_parameter(static_cast<uint8_t>(n), 0x05);
        if ((type & 0xFFu) != 1u) continue;
        g_state.function_group = static_cast<uint8_t>(n);
        (void)set_verb(g_state.function_group, 0x70500u);  // D0
        uint32_t nodes = get_parameter(g_state.function_group, 0x04);
        uint8_t start = static_cast<uint8_t>(nodes >> 16);
        uint8_t count = static_cast<uint8_t>(nodes);
        for (uint16_t w = start; w < start + count; ++w) {
            uint8_t nid = static_cast<uint8_t>(w);
            Widget& widget = g_state.widgets[nid];
            widget.present = true;
            widget.caps = get_parameter(nid, 0x09);
            read_connections(nid, widget);
            if (((widget.caps >> 20) & 0xFu) != kWidgetTypePin) continue;
            widget.pin_caps = get_parameter(nid, 0x0C);
            uint32_t config = 0;
            if (verb(codec, nid, 0xF1C00u, config)) widget.default_config = config;
        }
        constexpr int priorities[] = {30, 20, 10, 0};
        for (int priority : priorities) {
            for (uint16_t w = start; w < start + count; ++w) {
                uint8_t nid = static_cast<uint8_t>(w);
                if (pin_score(g_state.widgets[nid]) != priority) continue;
                bool visited[256]{};
                g_state.pin = nid;
                g_state.dac = 0;
                if (find_dac_path(g_state.pin, visited)) {
                    log_message(LogLevel::Info,
                                "hda: codec %u vendor=%08x AFG=%u output pin=%u DAC=%u",
                                codec, vendor, g_state.function_group,
                                g_state.pin, g_state.dac);
                    return true;
                }
            }
        }
    }
    log_message(LogLevel::Info, "hda: codec %u vendor=%08x has no analog output route",
                codec, vendor);
    return false;
}

size_t stream_base() { return 0x80 + static_cast<size_t>(g_state.stream_index) * 0x20; }

uint32_t read_stream_ctl(size_t sd) {
    return static_cast<uint32_t>(read16(sd)) |
           (static_cast<uint32_t>(read8(sd + 2)) << 16);
}

void write_stream_ctl(size_t sd, uint32_t value) {
    // SDCTL is 24-bit and SDSTS immediately follows it. A 32-bit access would
    // accidentally acknowledge status bits in the high byte.
    write16(sd, static_cast<uint16_t>(value));
    write8(sd + 2, static_cast<uint8_t>(value >> 16));
}

bool stop_stream() {
    size_t sd = stream_base();
    write_stream_ctl(sd, read_stream_ctl(sd) & ~kStreamCtlRun);
    for (uint32_t i = 0; i < 1000000; ++i) {
        if ((read_stream_ctl(sd) & kStreamCtlRun) == 0) return true;
        relax();
    }
    return false;
}

bool reset_stream() {
    size_t sd = stream_base();
    if (!stop_stream()) return false;
    write_stream_ctl(sd, read_stream_ctl(sd) | kStreamCtlReset);
    for (uint32_t i = 0; i < 1000000; ++i) {
        if (read_stream_ctl(sd) & kStreamCtlReset) break;
        if (i == 999999) return false;
        relax();
    }
    write_stream_ctl(sd, read_stream_ctl(sd) & ~kStreamCtlReset);
    for (uint32_t i = 0; i < 1000000; ++i) {
        if ((read_stream_ctl(sd) & kStreamCtlReset) == 0) return true;
        relax();
    }
    return false;
}

bool setup_output() {
    uint16_t gcap = read16(0x00);
    uint8_t input_streams = static_cast<uint8_t>((gcap >> 8) & 0x0Fu);
    uint8_t output_streams = static_cast<uint8_t>((gcap >> 12) & 0x0Fu);
    if (output_streams == 0) return false;
    g_state.stream_index = input_streams;
    g_state.stream_tag = 1;

    g_state.dma_phys = memory::alloc_kernel_block_pages(kDmaPages);
    g_state.bdl_phys = memory::alloc_kernel_page();
    if (g_state.dma_phys == 0 || g_state.bdl_phys == 0) return false;
    g_state.dma = static_cast<uint8_t*>(paging_phys_to_virt(g_state.dma_phys));
    g_state.bdl = static_cast<BdlEntry*>(paging_phys_to_virt(g_state.bdl_phys));
    if (g_state.dma == nullptr || g_state.bdl == nullptr) return false;
    memset(g_state.dma, 0, kDmaBytes);
    memset(g_state.bdl, 0, kPageSize);
    if (!reset_stream()) return false;

    (void)set_verb(g_state.dac, 0x70500u);
    (void)set_verb(g_state.pin, 0x70500u);
    (void)set_verb(g_state.dac, 0x20000u | kPcmFormat);
    (void)set_verb(g_state.dac, 0x70600u | (g_state.stream_tag << 4));
    (void)set_verb(g_state.dac, 0x30000u | (1u << 15) | (1u << 13) |
                                      (1u << 12));
    (void)set_verb(g_state.pin, 0x70700u | 0xC0u);  // output + headphone drive
    if (g_state.widgets[g_state.pin].pin_caps & (1u << 16))
        (void)set_verb(g_state.pin, 0x70C02u);       // external amplifier

    size_t sd = stream_base();
    write16(sd + 0x12, kPcmFormat);
    write16(sd + 0x0C, 0);  // One BDL entry.
    write32(sd + 0x18, static_cast<uint32_t>(g_state.bdl_phys));
    write32(sd + 0x1C, static_cast<uint32_t>(g_state.bdl_phys >> 32));
    uint32_t ctl = read_stream_ctl(sd);
    ctl &= ~(0xFu << 20);
    ctl |= static_cast<uint32_t>(g_state.stream_tag) << 20;
    write_stream_ctl(sd, ctl);
    return true;
}

bool init_device(const pci::PciDevice& device) {
    uint64_t phys = bar0(device);
    if (phys == 0) return false;
    uint16_t command = pci::read_config16(device, 0x04);
    command |= (1u << 1) | (1u << 2);
    command &= ~(1u << 0);
    pci::write_config16(device, 0x04, command);
    g_state.regs = map_mmio(phys);
    if (g_state.regs == nullptr) return false;

    write32(0x08, read32(0x08) & ~kGctlReset);
    if (!wait32(0x08, kGctlReset, 0)) return false;
    for (uint32_t i = 0; i < 100000; ++i) relax();
    write32(0x08, read32(0x08) | kGctlReset);
    if (!wait32(0x08, kGctlReset, kGctlReset)) return false;
    for (uint32_t i = 0; i < 200000; ++i) relax();

    write32(0x20, 0);  // Global and stream interrupts remain polled for now.
    write8(0x4C, 0);   // Stop CORB/RIRB before using immediate commands.
    write8(0x5C, 0);

    uint16_t codecs = read16(0x0E);
    log_message(LogLevel::Info,
                "hda: controller %04x:%04x version=%u.%u codecs=%04x",
                device.vendor, device.device, read8(0x03), read8(0x02), codecs);
    for (uint8_t codec = 0; codec < 15; ++codec) {
        if ((codecs & (1u << codec)) == 0) continue;
        memset(g_state.widgets, 0, sizeof(g_state.widgets));
        if (enumerate_codec(codec) && setup_output()) return true;
    }
    return false;
}

}  // namespace

void register_driver() {
    (void)driver_registry::register_pci_driver(
        "intel-hda", kPciMatches, sizeof(kPciMatches) / sizeof(kPciMatches[0]), init);
}

void init() {
    if (g_state.initialized) return;
    g_state.initialized = true;
    const pci::PciDevice* devices = pci::devices();
    for (size_t i = 0; i < pci::device_count(); ++i) {
        if (devices[i].class_code != 0x04 || devices[i].subclass != 0x03) continue;
        g_state.device = devices[i];
        if (init_device(devices[i])) {
            g_state.active = true;
            log_message(LogLevel::Info,
                        "hda: PCM output ready (48000 Hz, signed 16-bit stereo)");
            return;
        }
    }
    log_message(LogLevel::Warn, "hda: no usable output codec found");
}

bool available() { return g_state.active; }

size_t write_pcm(const void* data, size_t bytes) {
    if (!g_state.active || data == nullptr || bytes < 4) return 0;
    bytes &= ~static_cast<size_t>(3);
    while (__atomic_test_and_set(&g_state.lock, __ATOMIC_ACQUIRE)) relax();
    size_t done = 0;
    while (done < bytes) {
        size_t chunk = bytes - done;
        if (chunk > kDmaBytes) chunk = kDmaBytes;
        chunk &= ~static_cast<size_t>(3);
        if (!stop_stream()) break;
        memcpy(g_state.dma, static_cast<const uint8_t*>(data) + done, chunk);
        g_state.bdl[0].address_low = static_cast<uint32_t>(g_state.dma_phys);
        g_state.bdl[0].address_high = static_cast<uint32_t>(g_state.dma_phys >> 32);
        g_state.bdl[0].length = static_cast<uint32_t>(chunk);
        g_state.bdl[0].flags = 1u;  // Interrupt on completion (status works polled).
        size_t sd = stream_base();
        write32(sd + 0x08, static_cast<uint32_t>(chunk));
        write8(sd + 0x03, 0x1Cu);
        asm volatile("mfence" ::: "memory");
        write_stream_ctl(sd, read_stream_ctl(sd) | kStreamCtlRun);
        bool complete = false;
        // Roughly generous for a full 64 KiB buffer (~341 ms at this format).
        for (uint32_t spins = 0; spins < 300000000; ++spins) {
            if (read8(sd + 0x03) & kStreamStsBcis) {
                complete = true;
                break;
            }
            relax();
        }
        (void)stop_stream();
        write8(sd + 0x03, 0x1Cu);
        if (!complete) break;
        done += chunk;
    }
    __atomic_clear(&g_state.lock, __ATOMIC_RELEASE);
    return done;
}

}  // namespace hda
