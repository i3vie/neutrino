#include "drivers/net/virtio_net.hpp"

#include "drivers/driver_registry.hpp"
#include "arch/x86_64/memory/paging.hpp"
#include "arch/x86_64/percpu.hpp"
#include "drivers/log/logging.hpp"
#include "drivers/pci/pci.hpp"
#include "kernel/scheduler.hpp"
#include "kernel/memory/physical_allocator.hpp"
#include "lib/mem.hpp"
#include "net/network.hpp"

namespace virtio_net {

namespace {

constexpr uint16_t kVirtioVendorId = 0x1AF4;
constexpr uint8_t kVirtioPciCapId = 0x09;

constexpr uint8_t kCfgTypeCommon = 1;
constexpr uint8_t kCfgTypeNotify = 2;
constexpr uint8_t kCfgTypeIsr = 3;
constexpr uint8_t kCfgTypeDevice = 4;

constexpr uint8_t kStatusAcknowledge = 1u << 0;
constexpr uint8_t kStatusDriver = 1u << 1;
constexpr uint8_t kStatusDriverOk = 1u << 2;
constexpr uint8_t kStatusFeaturesOk = 1u << 3;
constexpr uint8_t kStatusFailed = 1u << 7;

constexpr uint64_t kFeatureMac = 1ull << 5;
constexpr uint64_t kFeatureVersion1 = 1ull << 32;

constexpr uint16_t kQueueIndexRx = 0;
constexpr uint16_t kQueueIndexTx = 1;
constexpr uint16_t kMaxQueueSize = 64;
constexpr uint16_t kMsixVectorUnused = 0xFFFFu;

constexpr uint16_t kVirtqDescFlagWrite = 1u << 1;
constexpr size_t kPageSize = 4096;
constexpr size_t kPacketBufferSize = kPageSize;
constexpr size_t kVirtioNetTxHeaderSize = 12;
constexpr size_t kVirtioNetRxHeaderSize = 12;
constexpr uint64_t kMmioVirtBase = 0xFFFFE10000000000ull;
constexpr size_t kMmioWindowSize = 2ull * 1024 * 1024;

struct [[gnu::packed]] VirtioPciCommonCfg {
    uint32_t device_feature_select;
    uint32_t device_feature;
    uint32_t driver_feature_select;
    uint32_t driver_feature;
    uint16_t msix_config;
    uint16_t num_queues;
    uint8_t device_status;
    uint8_t config_generation;
    uint16_t queue_select;
    uint16_t queue_size;
    uint16_t queue_msix_vector;
    uint16_t queue_enable;
    uint16_t queue_notify_off;
    uint64_t queue_desc;
    uint64_t queue_driver;
    uint64_t queue_device;
};

struct [[gnu::packed]] VirtioNetConfig {
    uint8_t mac[6];
};

struct [[gnu::packed]] VirtqDesc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};

struct [[gnu::packed]] VirtqAvailHeader {
    uint16_t flags;
    uint16_t idx;
};

struct [[gnu::packed]] VirtqUsedElem {
    uint32_t id;
    uint32_t len;
};

struct [[gnu::packed]] VirtqUsedHeader {
    uint16_t flags;
    uint16_t idx;
};

struct VirtioCapability {
    uint8_t cfg_type;
    uint8_t bar;
    uint32_t offset;
    uint32_t length;
    uint32_t notify_multiplier;
    bool present;
};

struct Queue {
    uint16_t queue_index;
    uint16_t size;
    uint16_t next_avail_idx;
    uint16_t last_used_idx;
    uint64_t desc_phys;
    uint64_t avail_phys;
    uint64_t used_phys;
    VirtqDesc* desc;
    VirtqAvailHeader* avail_header;
    uint16_t* avail_ring;
    VirtqUsedHeader* used_header;
    VirtqUsedElem* used_ring;
    volatile uint16_t* notify;
    uint64_t buffer_phys[kMaxQueueSize];
    uint8_t* buffer_virt[kMaxQueueSize];
    bool buffer_in_use[kMaxQueueSize];
};

struct DriverState {
    bool initialized;
    bool active;
    bool has_mac;
    bool link_registered;
    pci::PciDevice device;
    volatile VirtioPciCommonCfg* common_cfg;
    volatile uint8_t* isr_cfg;
    volatile uint8_t* device_cfg;
    volatile uint8_t* notify_base;
    uint32_t notify_multiplier;
    uint8_t mac[6];
    Queue rx_queue;
    Queue tx_queue;
    net::LinkDevice link_device;
};

DriverState g_state{};
uint64_t g_mmio_next_virt = kMmioVirtBase;
constexpr driver_registry::PciMatch kPciMatches[] = {
    {
        .vendor = kVirtioVendorId,
        .device = driver_registry::kAnyDevice,
        .class_code = 0x02,
        .subclass = 0x00,
        .prog_if = driver_registry::kAnyProgIf,
    },
};

void compiler_barrier() {
    asm volatile("" : : : "memory");
}

char hex_digit(uint8_t value) {
    return (value < 10) ? static_cast<char>('0' + value)
                        : static_cast<char>('a' + (value - 10));
}

void format_mac_string(const uint8_t* mac, char* out, size_t out_size) {
    if (out == nullptr || out_size < 18) {
        return;
    }
    for (size_t i = 0; i < 6; ++i) {
        uint8_t byte = mac[i];
        out[i * 3] = hex_digit(static_cast<uint8_t>((byte >> 4) & 0xF));
        out[i * 3 + 1] = hex_digit(static_cast<uint8_t>(byte & 0xF));
        if (i != 5) {
            out[i * 3 + 2] = ':';
        }
    }
    out[17] = '\0';
}

size_t pages_for_bytes(size_t bytes) {
    return (bytes + kPageSize - 1) / kPageSize;
}

uint64_t align_down_u64(uint64_t value, uint64_t alignment) {
    return value & ~(alignment - 1);
}

uint64_t align_up_u64(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

uint64_t read_device_features(volatile VirtioPciCommonCfg& common_cfg) {
    common_cfg.device_feature_select = 0;
    compiler_barrier();
    uint64_t low = common_cfg.device_feature;
    common_cfg.device_feature_select = 1;
    compiler_barrier();
    uint64_t high = common_cfg.device_feature;
    return low | (high << 32);
}

void write_driver_features(volatile VirtioPciCommonCfg& common_cfg,
                           uint64_t features) {
    common_cfg.driver_feature_select = 0;
    common_cfg.driver_feature = static_cast<uint32_t>(features & 0xFFFFFFFFu);
    common_cfg.driver_feature_select = 1;
    common_cfg.driver_feature = static_cast<uint32_t>(features >> 32);
}

uint64_t pci_bar_base(const pci::PciDevice& device, uint8_t bar_index) {
    if (bar_index >= 6) {
        return 0;
    }

    uint8_t reg = static_cast<uint8_t>(0x10 + (bar_index * 4));
    uint32_t low = pci::read_config32(device, reg);
    if ((low & 0x1u) != 0) {
        return 0;
    }

    uint64_t base = static_cast<uint64_t>(low & ~0xFu);
    uint32_t bar_type = (low >> 1) & 0x3u;
    if (bar_type == 0x2u) {
        if (bar_index + 1 >= 6) {
            return 0;
        }
        uint32_t high = pci::read_config32(
            device, static_cast<uint8_t>(reg + 4));
        base |= static_cast<uint64_t>(high) << 32;
    }
    return base;
}

bool read_virtio_capability(const pci::PciDevice& device,
                            uint8_t cfg_type,
                            VirtioCapability& out) {
    uint16_t status = pci::read_config16(device, 0x06);
    if ((status & (1u << 4)) == 0) {
        return false;
    }

    uint8_t cap_ptr = static_cast<uint8_t>(pci::read_config8(device, 0x34) & ~0x3u);
    for (size_t guard = 0; cap_ptr >= 0x40 && guard < 48; ++guard) {
        uint8_t cap_id = pci::read_config8(device, cap_ptr + 0);
        uint8_t next = pci::read_config8(device, cap_ptr + 1);
        uint8_t cap_len = pci::read_config8(device, cap_ptr + 2);
        if (cap_id == kVirtioPciCapId && cap_len >= 16) {
            uint8_t type = pci::read_config8(device, cap_ptr + 3);
            if (type == cfg_type) {
                out.cfg_type = type;
                out.bar = pci::read_config8(device, cap_ptr + 4);
                out.offset = pci::read_config32(device, cap_ptr + 8);
                out.length = pci::read_config32(device, cap_ptr + 12);
                out.notify_multiplier =
                    (cap_len >= 20 && cfg_type == kCfgTypeNotify)
                        ? pci::read_config32(device, cap_ptr + 16)
                        : 0;
                out.present = true;
                return true;
            }
        }
        if (next == 0 || next == cap_ptr) {
            break;
        }
        cap_ptr = static_cast<uint8_t>(next & ~0x3u);
    }

    return false;
}

volatile uint8_t* map_capability_region(const pci::PciDevice& device,
                                        const VirtioCapability& cap) {
    if (!cap.present) {
        return nullptr;
    }

    uint64_t bar_base = pci_bar_base(device, cap.bar);
    if (bar_base == 0) {
        return nullptr;
    }

    uint64_t region_phys = bar_base + cap.offset;
    uint64_t page_phys = align_down_u64(region_phys, kPageSize);
    uint64_t page_end = align_up_u64(region_phys + cap.length, kPageSize);
    size_t page_count = static_cast<size_t>((page_end - page_phys) / kPageSize);
    if (page_count == 0) {
        page_count = 1;
    }

    uint64_t virt_base = g_mmio_next_virt;
    uint64_t virt_end =
        virt_base + static_cast<uint64_t>(page_count) * kPageSize;
    if (virt_end - kMmioVirtBase > kMmioWindowSize) {
        log_message(LogLevel::Warn,
                    "virtio-net: MMIO window exhausted while mapping BAR%u",
                    static_cast<unsigned int>(cap.bar));
        return nullptr;
    }

    const uint64_t mmio_flags =
        PAGE_FLAG_WRITE | PAGE_FLAG_WRITE_THROUGH | PAGE_FLAG_CACHE_DISABLE;
    for (size_t i = 0; i < page_count; ++i) {
        uint64_t phys = page_phys + static_cast<uint64_t>(i) * kPageSize;
        uint64_t virt = virt_base + static_cast<uint64_t>(i) * kPageSize;
        if (!paging_map_page(virt, phys, mmio_flags)) {
            log_message(LogLevel::Warn,
                        "virtio-net: failed to map MMIO page phys=%016llx",
                        static_cast<unsigned long long>(phys));
            return nullptr;
        }
    }

    g_mmio_next_virt = virt_end;
    return reinterpret_cast<volatile uint8_t*>(virt_base + (region_phys - page_phys));
}

uint64_t alloc_zeroed_pages(size_t page_count) {
    if (page_count == 0) {
        return 0;
    }
    uint64_t phys = memory::alloc_kernel_block_pages(page_count);
    if (phys == 0) {
        return 0;
    }
    void* virt = paging_phys_to_virt(phys);
    if (virt == nullptr) {
        return 0;
    }
    memset(virt, 0, page_count * kPageSize);
    return phys;
}

bool init_queue_memory(Queue& queue, uint16_t queue_index, uint16_t size) {
    if (size == 0 || size > kMaxQueueSize) {
        return false;
    }

    memset(&queue, 0, sizeof(queue));
    queue.queue_index = queue_index;
    queue.size = size;

    size_t desc_bytes = sizeof(VirtqDesc) * size;
    size_t avail_bytes = sizeof(VirtqAvailHeader) + sizeof(uint16_t) * size;
    size_t used_bytes = sizeof(VirtqUsedHeader) + sizeof(VirtqUsedElem) * size;

    queue.desc_phys = alloc_zeroed_pages(pages_for_bytes(desc_bytes));
    queue.avail_phys = alloc_zeroed_pages(pages_for_bytes(avail_bytes));
    queue.used_phys = alloc_zeroed_pages(pages_for_bytes(used_bytes));
    if (queue.desc_phys == 0 || queue.avail_phys == 0 || queue.used_phys == 0) {
        return false;
    }

    queue.desc = static_cast<VirtqDesc*>(paging_phys_to_virt(queue.desc_phys));
    queue.avail_header =
        static_cast<VirtqAvailHeader*>(paging_phys_to_virt(queue.avail_phys));
    queue.avail_ring = reinterpret_cast<uint16_t*>(
        static_cast<uint8_t*>(paging_phys_to_virt(queue.avail_phys)) +
        sizeof(VirtqAvailHeader));
    queue.used_header =
        static_cast<VirtqUsedHeader*>(paging_phys_to_virt(queue.used_phys));
    queue.used_ring = reinterpret_cast<VirtqUsedElem*>(
        static_cast<uint8_t*>(paging_phys_to_virt(queue.used_phys)) +
        sizeof(VirtqUsedHeader));

    for (uint16_t i = 0; i < size; ++i) {
        queue.buffer_phys[i] = memory::alloc_kernel_page();
        if (queue.buffer_phys[i] == 0) {
            return false;
        }
        queue.buffer_virt[i] = static_cast<uint8_t*>(
            paging_phys_to_virt(queue.buffer_phys[i]));
        if (queue.buffer_virt[i] == nullptr) {
            return false;
        }
        memset(queue.buffer_virt[i], 0, kPacketBufferSize);
        queue.buffer_in_use[i] = false;
    }

    return true;
}

bool configure_queue(DriverState& state, Queue& queue) {
    volatile VirtioPciCommonCfg& common = *state.common_cfg;
    common.queue_select = queue.queue_index;
    compiler_barrier();

    uint16_t max_size = common.queue_size;
    if (max_size == 0) {
        return false;
    }
    if (max_size < queue.size) {
        queue.size = max_size;
    }

    common.queue_size = queue.size;
    common.queue_msix_vector = kMsixVectorUnused;
    common.queue_desc = queue.desc_phys;
    common.queue_driver = queue.avail_phys;
    common.queue_device = queue.used_phys;

    uint16_t notify_off = common.queue_notify_off;
    queue.notify = reinterpret_cast<volatile uint16_t*>(
        const_cast<volatile uint8_t*>(state.notify_base) +
        static_cast<uint32_t>(notify_off) * state.notify_multiplier);

    common.queue_enable = 1;
    compiler_barrier();
    return common.queue_enable == 1;
}

void notify_queue(const Queue& queue) {
    if (queue.notify == nullptr) {
        return;
    }
    compiler_barrier();
    *queue.notify = queue.queue_index;
}

void submit_receive_buffers(Queue& queue) {
    for (uint16_t i = 0; i < queue.size; ++i) {
        queue.desc[i].addr = queue.buffer_phys[i];
        queue.desc[i].len = static_cast<uint32_t>(kPacketBufferSize);
        queue.desc[i].flags = kVirtqDescFlagWrite;
        queue.desc[i].next = 0;
        queue.avail_ring[queue.next_avail_idx % queue.size] = i;
        queue.buffer_in_use[i] = true;
        ++queue.next_avail_idx;
    }
    compiler_barrier();
    queue.avail_header->idx = queue.next_avail_idx;
    notify_queue(queue);
}

void recycle_receive_descriptor(Queue& queue, uint16_t descriptor_index) {
    if (descriptor_index >= queue.size) {
        return;
    }

    queue.avail_ring[queue.next_avail_idx % queue.size] = descriptor_index;
    ++queue.next_avail_idx;
    compiler_barrier();
    queue.avail_header->idx = queue.next_avail_idx;
    notify_queue(queue);
}

void poll_rx_queue(Queue& queue) {
    while (queue.last_used_idx != queue.used_header->idx) {
        const VirtqUsedElem& elem = queue.used_ring[queue.last_used_idx % queue.size];
        ++queue.last_used_idx;

        if (elem.id >= queue.size) {
            continue;
        }

        size_t payload_len = 0;
        if (elem.len > kVirtioNetRxHeaderSize) {
            payload_len = static_cast<size_t>(elem.len - kVirtioNetRxHeaderSize);
        }

        if (payload_len > 0) {
            net::receive_frame(&g_state.link_device,
                               queue.buffer_virt[elem.id] + kVirtioNetRxHeaderSize,
                               payload_len);
        }

        recycle_receive_descriptor(queue, static_cast<uint16_t>(elem.id));
    }
}

void poll_tx_queue(Queue& queue) {
    while (queue.last_used_idx != queue.used_header->idx) {
        const VirtqUsedElem& elem = queue.used_ring[queue.last_used_idx % queue.size];
        ++queue.last_used_idx;
        if (elem.id >= queue.size) {
            continue;
        }
        queue.buffer_in_use[elem.id] = false;
    }
}

bool load_mac_address(DriverState& state) {
    if (!state.has_mac || state.device_cfg == nullptr || state.common_cfg == nullptr) {
        return false;
    }

    for (size_t attempt = 0; attempt < 4; ++attempt) {
        uint8_t before = state.common_cfg->config_generation;
        for (size_t i = 0; i < sizeof(state.mac); ++i) {
            state.mac[i] = state.device_cfg[i];
        }
        compiler_barrier();
        uint8_t after = state.common_cfg->config_generation;
        if (before == after) {
            return true;
        }
    }

    return false;
}

void set_failed(DriverState& state) {
    if (state.common_cfg == nullptr) {
        return;
    }
    state.common_cfg->device_status |= kStatusFailed;
}

bool init_device(const pci::PciDevice& device) {
    VirtioCapability common_cap{};
    VirtioCapability notify_cap{};
    VirtioCapability isr_cap{};
    VirtioCapability device_cap{};

    if (!read_virtio_capability(device, kCfgTypeCommon, common_cap) ||
        !read_virtio_capability(device, kCfgTypeNotify, notify_cap) ||
        !read_virtio_capability(device, kCfgTypeDevice, device_cap)) {
        log_message(LogLevel::Warn,
                    "virtio-net: device %02u:%02u.%u lacks required modern PCI capabilities",
                    static_cast<unsigned int>(device.bus),
                    static_cast<unsigned int>(device.slot),
                    static_cast<unsigned int>(device.function));
        return false;
    }

    read_virtio_capability(device, kCfgTypeIsr, isr_cap);

    volatile VirtioPciCommonCfg* common_cfg =
        reinterpret_cast<volatile VirtioPciCommonCfg*>(
            const_cast<volatile uint8_t*>(map_capability_region(device, common_cap)));
    volatile uint8_t* notify_base = map_capability_region(device, notify_cap);
    volatile uint8_t* isr_cfg = map_capability_region(device, isr_cap);
    volatile uint8_t* device_cfg = map_capability_region(device, device_cap);

    if (common_cfg == nullptr || notify_base == nullptr || device_cfg == nullptr) {
        log_message(LogLevel::Warn,
                    "virtio-net: failed to map PCI capability regions");
        return false;
    }

    uint16_t command = pci::read_config16(device, 0x04);
    command |= static_cast<uint16_t>((1u << 1) | (1u << 2));
    pci::write_config16(device, 0x04, command);

    DriverState state{};
    state.device = device;
    state.common_cfg = common_cfg;
    state.isr_cfg = isr_cfg;
    state.device_cfg = device_cfg;
    state.notify_base = notify_base;
    state.notify_multiplier = notify_cap.notify_multiplier;

    common_cfg->device_status = 0;
    compiler_barrier();
    common_cfg->device_status = kStatusAcknowledge;
    common_cfg->device_status |= kStatusDriver;

    uint64_t device_features = read_device_features(*common_cfg);
    if ((device_features & kFeatureVersion1) == 0) {
        log_message(LogLevel::Warn,
                    "virtio-net: device does not advertise VIRTIO_F_VERSION_1");
        set_failed(state);
        return false;
    }

    uint64_t negotiated_features = kFeatureVersion1;
    if ((device_features & kFeatureMac) != 0) {
        negotiated_features |= kFeatureMac;
        state.has_mac = true;
    }

    write_driver_features(*common_cfg, negotiated_features);
    common_cfg->device_status |= kStatusFeaturesOk;
    compiler_barrier();
    if ((common_cfg->device_status & kStatusFeaturesOk) == 0) {
        log_message(LogLevel::Warn,
                    "virtio-net: device rejected negotiated features");
        set_failed(state);
        return false;
    }

    if (!init_queue_memory(state.rx_queue, kQueueIndexRx, kMaxQueueSize) ||
        !init_queue_memory(state.tx_queue, kQueueIndexTx, kMaxQueueSize) ||
        !configure_queue(state, state.rx_queue) ||
        !configure_queue(state, state.tx_queue)) {
        log_message(LogLevel::Warn, "virtio-net: failed to initialize virtqueues");
        set_failed(state);
        return false;
    }

    submit_receive_buffers(state.rx_queue);

    if (state.has_mac && !load_mac_address(state)) {
        log_message(LogLevel::Warn,
                    "virtio-net: MAC feature present but config reads were unstable");
    }

    common_cfg->device_status |= kStatusDriverOk;
    compiler_barrier();

    g_state = state;
    g_state.active = true;
    g_state.initialized = true;
    if (!scheduler::register_poll(poll)) {
        log_message(LogLevel::Warn, "virtio-net: failed to register deferred poll");
    }

    if (g_state.has_mac) {
        g_state.link_registered =
            net::register_link(g_state.link_device,
                               "virtio-net",
                               &g_state,
                               transmit_frame,
                               g_state.mac);
    }

    if (g_state.has_mac) {
        char mac_string[18];
        format_mac_string(g_state.mac, mac_string, sizeof(mac_string));
        log_message(LogLevel::Info,
                    "virtio-net: online at %02u:%02u.%u mac=%s",
                    static_cast<unsigned int>(device.bus),
                    static_cast<unsigned int>(device.slot),
                    static_cast<unsigned int>(device.function),
                    mac_string);
    } else {
        log_message(LogLevel::Info,
                    "virtio-net: online at %02u:%02u.%u",
                    static_cast<unsigned int>(device.bus),
                    static_cast<unsigned int>(device.slot),
                    static_cast<unsigned int>(device.function));
    }

    return true;
}

}  // namespace

void register_driver() {
    (void)driver_registry::register_pci_driver(
        "virtio-net",
        kPciMatches,
        sizeof(kPciMatches) / sizeof(kPciMatches[0]),
        init);
}

void init() {
    if (g_state.initialized) {
        return;
    }

    g_state.initialized = true;

    const pci::PciDevice* list = pci::devices();
    size_t count = pci::device_count();
    for (size_t i = 0; i < count; ++i) {
        const pci::PciDevice& dev = list[i];
        if (dev.vendor != kVirtioVendorId ||
            dev.class_code != 0x02 ||
            dev.subclass != 0x00) {
            continue;
        }

        if (init_device(dev)) {
            return;
        }
    }

    log_message(LogLevel::Info, "virtio-net: no supported device found");
}

void poll() {
    if (!g_state.active) {
        return;
    }

    poll_rx_queue(g_state.rx_queue);
    poll_tx_queue(g_state.tx_queue);

    if (g_state.isr_cfg != nullptr) {
        (void)*g_state.isr_cfg;
    }
}

bool available() {
    return g_state.active;
}

bool transmit_frame(void* context, const void* data, size_t length) {
    (void)context;
    return transmit(data, length);
}

const uint8_t* mac_address() {
    return g_state.has_mac ? g_state.mac : nullptr;
}

bool transmit(const void* data, size_t length) {
    if (!g_state.active || data == nullptr || length == 0) {
        return false;
    }
    if (length + kVirtioNetTxHeaderSize > kPacketBufferSize) {
        return false;
    }

    Queue& queue = g_state.tx_queue;
    poll_tx_queue(queue);

    uint16_t descriptor_index = queue.size;
    for (uint16_t i = 0; i < queue.size; ++i) {
        if (!queue.buffer_in_use[i]) {
            descriptor_index = i;
            break;
        }
    }
    if (descriptor_index >= queue.size) {
        return false;
    }

    memset(queue.buffer_virt[descriptor_index], 0, kVirtioNetTxHeaderSize);
    memcpy(queue.buffer_virt[descriptor_index] + kVirtioNetTxHeaderSize,
           data,
           length);

    queue.desc[descriptor_index].addr = queue.buffer_phys[descriptor_index];
    queue.desc[descriptor_index].len =
        static_cast<uint32_t>(kVirtioNetTxHeaderSize + length);
    queue.desc[descriptor_index].flags = 0;
    queue.desc[descriptor_index].next = 0;
    queue.buffer_in_use[descriptor_index] = true;

    queue.avail_ring[queue.next_avail_idx % queue.size] = descriptor_index;
    ++queue.next_avail_idx;
    compiler_barrier();
    queue.avail_header->idx = queue.next_avail_idx;
    notify_queue(queue);
    return true;
}

}  // namespace virtio_net
