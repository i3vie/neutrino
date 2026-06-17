#include "drivers/storage/ahci.hpp"

#include "arch/x86_64/memory/paging.hpp"
#include "drivers/log/logging.hpp"
#include "drivers/pci/pci.hpp"
#include "kernel/memory/physical_allocator.hpp"
#include "lib/mem.hpp"

namespace ahci {

namespace {

constexpr size_t kMaxControllers = 4;
constexpr size_t kMaxDevices = 32;
constexpr size_t kPortCount = 32;
constexpr size_t kCommandSlotCount = 32;
constexpr size_t kPrdtEntryCount = 32;
constexpr size_t kPageSize = 4096;
constexpr uint64_t kPageMask = kPageSize - 1;
constexpr uint64_t kMmioVirtBase = 0xFFFFE30000000000ull;
constexpr uint64_t kMmioWindowSize = 2ull * 1024 * 1024;
constexpr uint64_t kAbarMapLength = 0x2000;

constexpr uint8_t kPciCommandIoSpace = 1u << 0;
constexpr uint8_t kPciCommandMemorySpace = 1u << 1;
constexpr uint8_t kPciCommandBusMaster = 1u << 2;

constexpr uint32_t kGhcAe = 1u << 31;
constexpr uint32_t kBohcBos = 1u << 0;
constexpr uint32_t kBohcOos = 1u << 1;
constexpr uint32_t kBohcBb = 1u << 4;

constexpr uint32_t kPortCmdSt = 1u << 0;
constexpr uint32_t kPortCmdFre = 1u << 4;
constexpr uint32_t kPortCmdFr = 1u << 14;
constexpr uint32_t kPortCmdCr = 1u << 15;

constexpr uint32_t kPortSstsDetMask = 0x0Fu;
constexpr uint32_t kPortSstsDetPresent = 0x03u;
constexpr uint32_t kPortSstsIpmMask = 0x0F00u;
constexpr uint32_t kPortSstsIpmActive = 0x0100u;

constexpr uint32_t kPortSigAta = 0x00000101u;

constexpr uint32_t kPortTfdBusy = 1u << 7;
constexpr uint32_t kPortTfdDrq = 1u << 3;
constexpr uint32_t kPortIsTfes = 1u << 30;

constexpr uint8_t kFisTypeRegH2d = 0x27;
constexpr uint8_t kAtaCmdIdentify = 0xEC;
constexpr uint8_t kAtaCmdReadDmaExt = 0x25;
constexpr uint8_t kAtaCmdWriteDmaExt = 0x35;

struct [[gnu::packed]] HbaPort {
    uint32_t clb;
    uint32_t clbu;
    uint32_t fb;
    uint32_t fbu;
    uint32_t is;
    uint32_t ie;
    uint32_t cmd;
    uint32_t rsv0;
    uint32_t tfd;
    uint32_t sig;
    uint32_t ssts;
    uint32_t sctl;
    uint32_t serr;
    uint32_t sact;
    uint32_t ci;
    uint32_t sntf;
    uint32_t fbs;
    uint32_t rsv1[11];
    uint32_t vendor[4];
};

struct [[gnu::packed]] HbaMemory {
    uint32_t cap;
    uint32_t ghc;
    uint32_t is;
    uint32_t pi;
    uint32_t vs;
    uint32_t ccc_ctl;
    uint32_t ccc_pts;
    uint32_t em_loc;
    uint32_t em_ctl;
    uint32_t cap2;
    uint32_t bohc;
    uint8_t rsv[0xA0 - 0x2C];
    uint8_t vendor[0x100 - 0xA0];
    HbaPort ports[kPortCount];
};

struct [[gnu::packed]] HbaCommandHeader {
    uint8_t cfl : 5;
    uint8_t a : 1;
    uint8_t w : 1;
    uint8_t p : 1;
    uint8_t r : 1;
    uint8_t b : 1;
    uint8_t c : 1;
    uint8_t rsv0 : 1;
    uint8_t pmp : 4;
    uint16_t prdtl;
    volatile uint32_t prdbc;
    uint32_t ctba;
    uint32_t ctbau;
    uint32_t rsv1[4];
};

struct [[gnu::packed]] HbaPrdtEntry {
    uint32_t dba;
    uint32_t dbau;
    uint32_t rsv0;
    uint32_t dbc : 22;
    uint32_t rsv1 : 9;
    uint32_t i : 1;
};

struct [[gnu::packed]] HbaCommandTable {
    uint8_t cfis[64];
    uint8_t acmd[16];
    uint8_t rsv[48];
    HbaPrdtEntry prdt[kPrdtEntryCount];
};

struct [[gnu::packed]] FisRegH2d {
    uint8_t fis_type;
    uint8_t pmport : 4;
    uint8_t rsv0 : 3;
    uint8_t command_control : 1;
    uint8_t command;
    uint8_t featurel;
    uint8_t lba0;
    uint8_t lba1;
    uint8_t lba2;
    uint8_t device;
    uint8_t lba3;
    uint8_t lba4;
    uint8_t lba5;
    uint8_t featureh;
    uint8_t countl;
    uint8_t counth;
    uint8_t icc;
    uint8_t control;
    uint8_t rsv1[4];
};

struct ControllerState {
    bool used;
    pci::PciDevice pci_device;
    volatile HbaMemory* abar;
    uint64_t abar_phys;
};

struct DeviceState {
    bool used;
    bool present;
    IdentifyInfo identify;
    char name[16];
    ControllerState* controller;
    volatile HbaPort* port;
    uint8_t port_index;
    uint64_t command_list_phys;
    uint8_t* command_list_virt;
    uint64_t fis_phys;
    uint8_t* fis_virt;
    uint64_t command_table_phys;
    HbaCommandTable* command_table;
    uint64_t identify_buffer_phys;
    uint16_t* identify_buffer;
    volatile int lock;
};

ControllerState g_controllers[kMaxControllers]{};
DeviceState g_devices[kMaxDevices]{};
size_t g_device_count = 0;
bool g_initialized = false;
uint64_t g_mmio_next_virt = kMmioVirtBase;

constexpr IdentifyInfo kEmptyIdentifyInfo = {false, "", 0};

uint64_t align_down_u64(uint64_t value, uint64_t alignment) {
    return value & ~(alignment - 1);
}

uint64_t align_up_u64(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

void cpu_relax() {
    asm volatile("pause");
}

void lock_device(DeviceState& device) {
    while (__atomic_test_and_set(&device.lock, __ATOMIC_ACQUIRE)) {
        cpu_relax();
    }
}

void unlock_device(DeviceState& device) {
    __atomic_clear(&device.lock, __ATOMIC_RELEASE);
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
        uint32_t high = pci::read_config32(device, static_cast<uint8_t>(reg + 4));
        base |= static_cast<uint64_t>(high) << 32;
    }
    return base;
}

volatile uint8_t* map_mmio_range(uint64_t phys_base, uint64_t length) {
    if (phys_base == 0 || length == 0) {
        return nullptr;
    }

    uint64_t page_phys = align_down_u64(phys_base, kPageSize);
    uint64_t page_end = align_up_u64(phys_base + length, kPageSize);
    size_t page_count =
        static_cast<size_t>((page_end - page_phys) / kPageSize);
    if (page_count == 0) {
        page_count = 1;
    }

    uint64_t virt_base = g_mmio_next_virt;
    uint64_t virt_end =
        virt_base + static_cast<uint64_t>(page_count) * kPageSize;
    if (virt_end - kMmioVirtBase > kMmioWindowSize) {
        log_message(LogLevel::Warn,
                    "AHCI: MMIO window exhausted while mapping %016llx",
                    static_cast<unsigned long long>(phys_base));
        return nullptr;
    }

    const uint64_t flags =
        PAGE_FLAG_WRITE | PAGE_FLAG_WRITE_THROUGH | PAGE_FLAG_CACHE_DISABLE;
    for (size_t i = 0; i < page_count; ++i) {
        uint64_t phys = page_phys + static_cast<uint64_t>(i) * kPageSize;
        uint64_t virt = virt_base + static_cast<uint64_t>(i) * kPageSize;
        if (!paging_map_page(virt, phys, flags)) {
            log_message(LogLevel::Warn,
                        "AHCI: failed to map MMIO page phys=%016llx",
                        static_cast<unsigned long long>(phys));
            return nullptr;
        }
    }

    g_mmio_next_virt = virt_end;
    return reinterpret_cast<volatile uint8_t*>(virt_base + (phys_base - page_phys));
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
        memory::free_kernel_block(phys);
        return 0;
    }
    memset(virt, 0, page_count * kPageSize);
    return phys;
}

bool wait_port_idle(volatile HbaPort& port, uint32_t spin_count = 1000000) {
    while (spin_count-- > 0) {
        if ((port.tfd & (kPortTfdBusy | kPortTfdDrq)) == 0) {
            return true;
        }
        cpu_relax();
    }
    return false;
}

bool stop_port(volatile HbaPort& port) {
    port.cmd &= ~kPortCmdSt;
    port.cmd &= ~kPortCmdFre;
    for (uint32_t spins = 0; spins < 1000000; ++spins) {
        if ((port.cmd & (kPortCmdCr | kPortCmdFr)) == 0) {
            return true;
        }
        cpu_relax();
    }
    return false;
}

void start_port(volatile HbaPort& port) {
    port.cmd |= kPortCmdFre;
    port.cmd |= kPortCmdSt;
}

bool port_has_sata_device(const volatile HbaPort& port) {
    uint32_t ssts = port.ssts;
    uint32_t det = ssts & kPortSstsDetMask;
    uint32_t ipm = ssts & kPortSstsIpmMask;
    if (det != kPortSstsDetPresent) {
        return false;
    }
    if (ipm != kPortSstsIpmActive) {
        return false;
    }
    return port.sig == kPortSigAta;
}

void swap_bytes(char* dest, const uint16_t* src, size_t word_count) {
    for (size_t i = 0; i < word_count; ++i) {
        dest[i * 2] = static_cast<char>(src[i] >> 8);
        dest[i * 2 + 1] = static_cast<char>(src[i] & 0xFF);
    }
}

void trim_string(char* str, size_t len) {
    for (size_t i = len; i > 0; --i) {
        if (str[i - 1] == ' ' || str[i - 1] == '\0') {
            str[i - 1] = '\0';
        } else {
            break;
        }
    }
}

uint64_t identify_sector_count(const uint16_t* identify) {
    uint64_t lba48 =
        static_cast<uint64_t>(identify[100]) |
        (static_cast<uint64_t>(identify[101]) << 16) |
        (static_cast<uint64_t>(identify[102]) << 32) |
        (static_cast<uint64_t>(identify[103]) << 48);
    if (lba48 != 0) {
        return lba48;
    }

    return static_cast<uint64_t>(identify[60]) |
           (static_cast<uint64_t>(identify[61]) << 16);
}

bool build_prdt(HbaCommandTable& table,
                const void* buffer,
                size_t byte_count,
                uint16_t& prdt_count) {
    prdt_count = 0;
    if (byte_count == 0) {
        return false;
    }

    uintptr_t virt = reinterpret_cast<uintptr_t>(buffer);
    size_t remaining = byte_count;

    while (remaining > 0) {
        if (prdt_count >= kPrdtEntryCount) {
            return false;
        }

        uint64_t phys = 0;
        if (!paging_resolve_cr3(paging_kernel_cr3(), virt, phys)) {
            return false;
        }

        size_t page_offset = static_cast<size_t>(virt & kPageMask);
        size_t chunk = kPageSize - page_offset;
        if (chunk > remaining) {
            chunk = remaining;
        }

        HbaPrdtEntry& entry = table.prdt[prdt_count];
        entry = {};
        entry.dba = static_cast<uint32_t>(phys & 0xFFFFFFFFu);
        entry.dbau = static_cast<uint32_t>(phys >> 32);
        entry.dbc = static_cast<uint32_t>(chunk - 1);
        entry.i = 1;

        virt += chunk;
        remaining -= chunk;
        ++prdt_count;
    }

    return true;
}

bool issue_command(DeviceState& device,
                   uint8_t command,
                   uint64_t lba,
                   uint16_t sector_count,
                   void* buffer,
                   bool is_write) {
    if (device.port == nullptr || sector_count == 0 || buffer == nullptr) {
        return false;
    }

    volatile HbaPort& port = *device.port;
    if (!wait_port_idle(port)) {
        log_message(LogLevel::Warn,
                    "AHCI: port %u busy before %s lba=%llu count=%u tfd=%08x is=%08x serr=%08x ci=%08x",
                    device.port_index,
                    is_write ? "write" : "read",
                    static_cast<unsigned long long>(lba),
                    static_cast<unsigned>(sector_count),
                    port.tfd,
                    port.is,
                    port.serr,
                    port.ci);
        return false;
    }

    port.is = 0xFFFFFFFFu;
    port.serr = 0xFFFFFFFFu;

    auto* headers =
        reinterpret_cast<HbaCommandHeader*>(device.command_list_virt);
    HbaCommandHeader& header = headers[0];
    header = {};
    header.cfl = sizeof(FisRegH2d) / sizeof(uint32_t);
    header.w = is_write ? 1u : 0u;

    HbaCommandTable& table = *device.command_table;
    memset(&table, 0, sizeof(table));

    uint16_t prdt_count = 0;
    size_t byte_count = static_cast<size_t>(sector_count) * 512;
    if (!build_prdt(table, buffer, byte_count, prdt_count)) {
        return false;
    }
    header.prdtl = prdt_count;
    header.ctba = static_cast<uint32_t>(device.command_table_phys & 0xFFFFFFFFu);
    header.ctbau = static_cast<uint32_t>(device.command_table_phys >> 32);

    auto* fis = reinterpret_cast<FisRegH2d*>(table.cfis);
    memset(fis, 0, sizeof(FisRegH2d));
    fis->fis_type = kFisTypeRegH2d;
    fis->command_control = 1;
    fis->command = command;
    fis->device = 1u << 6;
    fis->lba0 = static_cast<uint8_t>(lba);
    fis->lba1 = static_cast<uint8_t>(lba >> 8);
    fis->lba2 = static_cast<uint8_t>(lba >> 16);
    fis->lba3 = static_cast<uint8_t>(lba >> 24);
    fis->lba4 = static_cast<uint8_t>(lba >> 32);
    fis->lba5 = static_cast<uint8_t>(lba >> 40);
    fis->countl = static_cast<uint8_t>(sector_count);
    fis->counth = static_cast<uint8_t>(sector_count >> 8);

    port.ci = 1u;
    for (uint32_t spins = 0; spins < 4000000; ++spins) {
        if ((port.ci & 1u) == 0) {
            if ((port.is & kPortIsTfes) != 0) {
                log_message(LogLevel::Warn,
                            "AHCI: %s failed lba=%llu count=%u tfd=%08x is=%08x serr=%08x sact=%08x",
                            is_write ? "write" : "read",
                            static_cast<unsigned long long>(lba),
                            static_cast<unsigned>(sector_count),
                            port.tfd,
                            port.is,
                            port.serr,
                            port.sact);
                return false;
            }
            return true;
        }
        if ((port.is & kPortIsTfes) != 0) {
            log_message(LogLevel::Warn,
                        "AHCI: %s taskfile error lba=%llu count=%u tfd=%08x is=%08x serr=%08x sact=%08x ci=%08x",
                        is_write ? "write" : "read",
                        static_cast<unsigned long long>(lba),
                        static_cast<unsigned>(sector_count),
                        port.tfd,
                        port.is,
                        port.serr,
                        port.sact,
                        port.ci);
            return false;
        }
        cpu_relax();
    }

    log_message(LogLevel::Warn,
                "AHCI: %s timeout lba=%llu count=%u tfd=%08x is=%08x serr=%08x sact=%08x ci=%08x",
                is_write ? "write" : "read",
                static_cast<unsigned long long>(lba),
                static_cast<unsigned>(sector_count),
                port.tfd,
                port.is,
                port.serr,
                port.sact,
                port.ci);
    return false;
}

bool init_device(DeviceState& device) {
    volatile HbaPort& port = *device.port;

    if (!stop_port(port)) {
        return false;
    }

    device.command_list_phys = alloc_zeroed_pages(1);
    device.fis_phys = alloc_zeroed_pages(1);
    device.command_table_phys = alloc_zeroed_pages(1);
    device.identify_buffer_phys = alloc_zeroed_pages(1);
    if (device.command_list_phys == 0 || device.fis_phys == 0 ||
        device.command_table_phys == 0 || device.identify_buffer_phys == 0) {
        return false;
    }

    device.command_list_virt = static_cast<uint8_t*>(
        paging_phys_to_virt(device.command_list_phys));
    device.fis_virt = static_cast<uint8_t*>(paging_phys_to_virt(device.fis_phys));
    device.command_table = static_cast<HbaCommandTable*>(
        paging_phys_to_virt(device.command_table_phys));
    device.identify_buffer = static_cast<uint16_t*>(
        paging_phys_to_virt(device.identify_buffer_phys));
    if (device.command_list_virt == nullptr || device.fis_virt == nullptr ||
        device.command_table == nullptr || device.identify_buffer == nullptr) {
        return false;
    }

    memset(device.command_list_virt, 0, kPageSize);
    memset(device.fis_virt, 0, kPageSize);
    memset(device.command_table, 0, kPageSize);
    memset(device.identify_buffer, 0, kPageSize);

    port.clb = static_cast<uint32_t>(device.command_list_phys & 0xFFFFFFFFu);
    port.clbu = static_cast<uint32_t>(device.command_list_phys >> 32);
    port.fb = static_cast<uint32_t>(device.fis_phys & 0xFFFFFFFFu);
    port.fbu = static_cast<uint32_t>(device.fis_phys >> 32);
    port.is = 0xFFFFFFFFu;
    port.ie = 0;
    port.serr = 0xFFFFFFFFu;

    start_port(port);

    if (!issue_command(device,
                       kAtaCmdIdentify,
                       0,
                       1,
                       device.identify_buffer,
                       false)) {
        stop_port(port);
        return false;
    }

    swap_bytes(device.identify.model, device.identify_buffer + 27, 20);
    device.identify.model[40] = '\0';
    trim_string(device.identify.model, 40);
    device.identify.sector_count = identify_sector_count(device.identify_buffer);
    device.identify.present = device.identify.sector_count != 0;
    device.present = device.identify.present;
    return device.present;
}

bool take_bios_ownership(volatile HbaMemory& abar) {
    if ((abar.cap2 & 1u) == 0) {
        return true;
    }

    abar.bohc |= kBohcOos;
    for (uint32_t spins = 0; spins < 1000000; ++spins) {
        uint32_t bohc = abar.bohc;
        if ((bohc & (kBohcBos | kBohcBb)) == 0) {
            return true;
        }
        cpu_relax();
    }
    return (abar.bohc & kBohcBos) == 0;
}

void enable_pci_bus_mastering(const pci::PciDevice& device) {
    uint16_t command = pci::read_config16(device, 0x04);
    command |= static_cast<uint16_t>(kPciCommandIoSpace |
                                     kPciCommandMemorySpace |
                                     kPciCommandBusMaster);
    pci::write_config16(device, 0x04, command);
}

void format_device_name(char* buffer, size_t buffer_size, size_t index) {
    if (buffer == nullptr || buffer_size < 7) {
        return;
    }

    size_t pos = 0;
    const char* prefix = "AHCI_";
    while (prefix[pos] != '\0' && pos + 1 < buffer_size) {
        buffer[pos] = prefix[pos];
        ++pos;
    }

    char digits[10];
    size_t digit_count = 0;
    do {
        digits[digit_count++] = static_cast<char>('0' + (index % 10));
        index /= 10;
    } while (index > 0 && digit_count < sizeof(digits));

    for (size_t i = 0; i < digit_count && pos + 1 < buffer_size; ++i) {
        buffer[pos++] = digits[digit_count - 1 - i];
    }
    buffer[pos] = '\0';
}

void probe_controller(const pci::PciDevice& pci_device) {
    if (g_device_count >= kMaxDevices) {
        return;
    }

    ControllerState* controller = nullptr;
    for (auto& slot : g_controllers) {
        if (!slot.used) {
            controller = &slot;
            break;
        }
    }
    if (controller == nullptr) {
        log_message(LogLevel::Warn, "AHCI: controller table full");
        return;
    }

    uint64_t abar_phys = pci_bar_base(pci_device, 5);
    if (abar_phys == 0) {
        log_message(LogLevel::Warn,
                    "AHCI: %02u:%02u.%u missing ABAR",
                    static_cast<unsigned int>(pci_device.bus),
                    static_cast<unsigned int>(pci_device.slot),
                    static_cast<unsigned int>(pci_device.function));
        return;
    }

    enable_pci_bus_mastering(pci_device);
    auto* abar = reinterpret_cast<volatile HbaMemory*>(
        const_cast<volatile uint8_t*>(map_mmio_range(abar_phys, kAbarMapLength)));
    if (abar == nullptr) {
        return;
    }

    abar->ghc |= kGhcAe;
    if (!take_bios_ownership(*abar)) {
        log_message(LogLevel::Warn,
                    "AHCI: BIOS ownership handoff timed out on %02u:%02u.%u",
                    static_cast<unsigned int>(pci_device.bus),
                    static_cast<unsigned int>(pci_device.slot),
                    static_cast<unsigned int>(pci_device.function));
    }

    controller->used = true;
    controller->pci_device = pci_device;
    controller->abar = abar;
    controller->abar_phys = abar_phys;

    uint32_t implemented = abar->pi;
    for (size_t port_index = 0; port_index < kPortCount; ++port_index) {
        if ((implemented & (1u << port_index)) == 0) {
            continue;
        }
        if (g_device_count >= kMaxDevices) {
            log_message(LogLevel::Warn, "AHCI: device table full");
            break;
        }

        volatile HbaPort& port = abar->ports[port_index];
        if (!port_has_sata_device(port)) {
            continue;
        }

        DeviceState& device = g_devices[g_device_count];
        memset(&device, 0, sizeof(device));
        device.used = true;
        device.controller = controller;
        device.port = &port;
        device.port_index = static_cast<uint8_t>(port_index);
        format_device_name(device.name, sizeof(device.name), g_device_count);

        if (!init_device(device)) {
            log_message(LogLevel::Warn,
                        "AHCI: failed to initialize disk on %02u:%02u.%u port %u",
                        static_cast<unsigned int>(pci_device.bus),
                        static_cast<unsigned int>(pci_device.slot),
                        static_cast<unsigned int>(pci_device.function),
                        static_cast<unsigned int>(port_index));
            continue;
        }

        log_message(LogLevel::Info,
                    "AHCI: disk %s controller=%02u:%02u.%u port=%u model='%s' sectors=%llu",
                    device.name,
                    static_cast<unsigned int>(pci_device.bus),
                    static_cast<unsigned int>(pci_device.slot),
                    static_cast<unsigned int>(pci_device.function),
                    static_cast<unsigned int>(port_index),
                    device.identify.model,
                    static_cast<unsigned long long>(device.identify.sector_count));

        ++g_device_count;
    }
}

void probe_controllers() {
    const pci::PciDevice* devices = pci::devices();
    size_t count = pci::device_count();
    for (size_t i = 0; i < count; ++i) {
        const pci::PciDevice& device = devices[i];
        if (device.class_code == 0x01 && device.subclass == 0x06 &&
            device.prog_if == 0x01) {
            probe_controller(device);
        }
    }
}

DeviceState* get_device(size_t device_index) {
    if (device_index >= g_device_count) {
        return nullptr;
    }
    if (!g_devices[device_index].used || !g_devices[device_index].present) {
        return nullptr;
    }
    return &g_devices[device_index];
}

Status do_rw(DeviceState& device, uint64_t lba, uint8_t sector_count,
             void* buffer, bool is_write) {
    if (!device.present) {
        return Status::NoDevice;
    }
    if (sector_count == 0 || buffer == nullptr) {
        return Status::IoError;
    }

    uint64_t last_lba = lba + static_cast<uint64_t>(sector_count);
    if (lba >= device.identify.sector_count ||
        last_lba > device.identify.sector_count) {
        return Status::IoError;
    }

    lock_device(device);
    bool ok = issue_command(device,
                            is_write ? kAtaCmdWriteDmaExt : kAtaCmdReadDmaExt,
                            lba,
                            sector_count,
                            buffer,
                            is_write);
    unlock_device(device);
    return ok ? Status::Ok : Status::IoError;
}

}  // namespace

bool init() {
    if (g_initialized) {
        return g_device_count != 0;
    }

    g_initialized = true;
    g_device_count = 0;
    g_mmio_next_virt = kMmioVirtBase;
    memset(g_controllers, 0, sizeof(g_controllers));
    memset(g_devices, 0, sizeof(g_devices));

    probe_controllers();
    return g_device_count != 0;
}

size_t device_count() {
    init();
    return g_device_count;
}

const IdentifyInfo& identify(size_t device_index) {
    init();
    DeviceState* device = get_device(device_index);
    if (device == nullptr) {
        return kEmptyIdentifyInfo;
    }
    return device->identify;
}

const char* device_name(size_t device_index) {
    init();
    DeviceState* device = get_device(device_index);
    if (device == nullptr) {
        return nullptr;
    }
    return device->name;
}

Status read_sectors(size_t device_index, uint64_t lba, uint8_t sector_count,
                    void* buffer) {
    init();
    DeviceState* device = get_device(device_index);
    if (device == nullptr) {
        return Status::NoDevice;
    }
    return do_rw(*device, lba, sector_count, buffer, false);
}

Status write_sectors(size_t device_index, uint64_t lba, uint8_t sector_count,
                     const void* buffer) {
    init();
    DeviceState* device = get_device(device_index);
    if (device == nullptr) {
        return Status::NoDevice;
    }
    return do_rw(*device,
                 lba,
                 sector_count,
                 const_cast<void*>(buffer),
                 true);
}

}  // namespace ahci
