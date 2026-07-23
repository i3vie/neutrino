#include "drivers/net/e1000e.hpp"

#include "drivers/driver_registry.hpp"
#include "arch/x86_64/memory/paging.hpp"
#include "arch/x86_64/percpu.hpp"
#include "arch/x86_64/lapic.hpp"
#include "drivers/log/logging.hpp"
#include "drivers/pci/pci.hpp"
#include "kernel/interrupts.hpp"
#include "kernel/module.hpp"
#include "kernel/scheduler.hpp"
#include "kernel/memory/physical_allocator.hpp"
#include "kernel/sync.hpp"
#include "lib/mem.hpp"
#include "net/network.hpp"

namespace e1000e {

namespace {

#ifdef NEUTRINO_DYNAMIC_MODULE_E1000E
const kernel_module::Api* g_module_api = nullptr;
#endif

constexpr uint16_t kIntelVendorId = 0x8086;

enum DeviceFlags : uint8_t {
    DEVICE_PCH_LAN = 1u << 0,
    DEVICE_I219 = 1u << 1,
    DEVICE_DMA_BURST = 1u << 2,
    DEVICE_DISABLE_ASPM_L0S = 1u << 3,
    DEVICE_DISABLE_ASPM_L1 = 1u << 4,
};

struct DeviceProfile {
    uint16_t device_id;
    const char* name;
    uint8_t flags;
    uint8_t pba_kib;
};

#define E1000E_SUPPORTED_DEVICES(X) \
    X(0x10D3, "82574L", DEVICE_DMA_BURST | DEVICE_DISABLE_ASPM_L0S | DEVICE_DISABLE_ASPM_L1, 32) \
    X(0x10F6, "82574LA", DEVICE_DMA_BURST | DEVICE_DISABLE_ASPM_L0S | DEVICE_DISABLE_ASPM_L1, 32) \
    X(0x150C, "82583V", DEVICE_DISABLE_ASPM_L0S | DEVICE_DISABLE_ASPM_L1, 32) \
    X(0x153A, "I217-LM", DEVICE_PCH_LAN, 26) \
    X(0x153B, "I217-V", DEVICE_PCH_LAN, 26) \
    X(0x1559, "I218-V", DEVICE_PCH_LAN, 26) \
    X(0x155A, "I218-LM", DEVICE_PCH_LAN, 26) \
    X(0x156F, "I219-LM", DEVICE_PCH_LAN | DEVICE_I219, 26) \
    X(0x1570, "I219-V", DEVICE_PCH_LAN | DEVICE_I219, 26) \
    X(0x15A0, "I218-LM2", DEVICE_PCH_LAN, 26) \
    X(0x15A1, "I218-V2", DEVICE_PCH_LAN, 26) \
    X(0x15A2, "I218-LM3", DEVICE_PCH_LAN, 26) \
    X(0x15A3, "I218-V3", DEVICE_PCH_LAN, 26) \
    X(0x15B7, "I219-LM2", DEVICE_PCH_LAN | DEVICE_I219, 26) \
    X(0x15B8, "I219-V2", DEVICE_PCH_LAN | DEVICE_I219, 26) \
    X(0x15B9, "I219-LM3", DEVICE_PCH_LAN | DEVICE_I219, 26) \
    X(0x15BB, "I219-LM7", DEVICE_PCH_LAN | DEVICE_I219, 26) \
    X(0x15BC, "I219-V7", DEVICE_PCH_LAN | DEVICE_I219, 26) \
    X(0x15BD, "I219-LM6", DEVICE_PCH_LAN | DEVICE_I219, 26) \
    X(0x15BE, "I219-V6", DEVICE_PCH_LAN | DEVICE_I219, 26) \
    X(0x15D6, "I219-V5", DEVICE_PCH_LAN | DEVICE_I219, 26) \
    X(0x15D7, "I219-LM4", DEVICE_PCH_LAN | DEVICE_I219, 26) \
    X(0x15D8, "I219-V4", DEVICE_PCH_LAN | DEVICE_I219, 26) \
    X(0x15DF, "I219-LM8", DEVICE_PCH_LAN | DEVICE_I219, 26) \
    X(0x15E0, "I219-V8", DEVICE_PCH_LAN | DEVICE_I219, 26) \
    X(0x15E1, "I219-LM9", DEVICE_PCH_LAN | DEVICE_I219, 26) \
    X(0x15E2, "I219-V9", DEVICE_PCH_LAN | DEVICE_I219, 26) \
    X(0x15E3, "I219-LM5", DEVICE_PCH_LAN | DEVICE_I219, 26) \
    X(0x15F4, "I219-LM15", DEVICE_PCH_LAN | DEVICE_I219, 26) \
    X(0x15F5, "I219-V15", DEVICE_PCH_LAN | DEVICE_I219, 26) \
    X(0x15F9, "I219-LM14", DEVICE_PCH_LAN | DEVICE_I219, 26) \
    X(0x15FA, "I219-V14", DEVICE_PCH_LAN | DEVICE_I219, 26) \
    X(0x15FB, "I219-LM13", DEVICE_PCH_LAN | DEVICE_I219, 26) \
    X(0x15FC, "I219-V13", DEVICE_PCH_LAN | DEVICE_I219, 26) \
    X(0x0D4C, "I219-LM11", DEVICE_PCH_LAN | DEVICE_I219, 26) \
    X(0x0D4D, "I219-V11", DEVICE_PCH_LAN | DEVICE_I219, 26) \
    X(0x0D4E, "I219-LM10", DEVICE_PCH_LAN | DEVICE_I219, 26) \
    X(0x0D4F, "I219-V10", DEVICE_PCH_LAN | DEVICE_I219, 26) \
    X(0x0D53, "I219-LM12", DEVICE_PCH_LAN | DEVICE_I219, 26) \
    X(0x0D55, "I219-V12", DEVICE_PCH_LAN | DEVICE_I219, 26) \
    X(0x0DC5, "I219-LM23", DEVICE_PCH_LAN | DEVICE_I219, 26) \
    X(0x0DC6, "I219-V23", DEVICE_PCH_LAN | DEVICE_I219, 26) \
    X(0x0DC7, "I219-LM22", DEVICE_PCH_LAN | DEVICE_I219, 26) \
    X(0x0DC8, "I219-V22", DEVICE_PCH_LAN | DEVICE_I219, 26) \
    X(0x1A1C, "I219-LM17", DEVICE_PCH_LAN | DEVICE_I219, 26) \
    X(0x1A1D, "I219-V17", DEVICE_PCH_LAN | DEVICE_I219, 26) \
    X(0x1A1E, "I219-LM16", DEVICE_PCH_LAN | DEVICE_I219, 26) \
    X(0x1A1F, "I219-V16", DEVICE_PCH_LAN | DEVICE_I219, 26) \
    X(0x550A, "I219-LM18", DEVICE_PCH_LAN | DEVICE_I219, 26) \
    X(0x550B, "I219-V18", DEVICE_PCH_LAN | DEVICE_I219, 26) \
    X(0x550C, "I219-LM19", DEVICE_PCH_LAN | DEVICE_I219, 26) \
    X(0x550D, "I219-V19", DEVICE_PCH_LAN | DEVICE_I219, 26) \
    X(0x550E, "I219-LM20", DEVICE_PCH_LAN | DEVICE_I219, 26) \
    X(0x550F, "I219-V20", DEVICE_PCH_LAN | DEVICE_I219, 26) \
    X(0x5510, "I219-LM21", DEVICE_PCH_LAN | DEVICE_I219, 26) \
    X(0x5511, "I219-V21", DEVICE_PCH_LAN | DEVICE_I219, 26) \
    X(0x57A0, "I219-LM24", DEVICE_PCH_LAN | DEVICE_I219, 26) \
    X(0x57A1, "I219-V24", DEVICE_PCH_LAN | DEVICE_I219, 26) \
    X(0x57B3, "I219-LM25", DEVICE_PCH_LAN | DEVICE_I219, 26) \
    X(0x57B4, "I219-V25", DEVICE_PCH_LAN | DEVICE_I219, 26) \
    X(0x57B7, "I219-LM27", DEVICE_PCH_LAN | DEVICE_I219, 26) \
    X(0x57B8, "I219-V27", DEVICE_PCH_LAN | DEVICE_I219, 26) \
    X(0x57B9, "I219-LM29", DEVICE_PCH_LAN | DEVICE_I219, 26) \
    X(0x57BA, "I219-V29", DEVICE_PCH_LAN | DEVICE_I219, 26)

constexpr DeviceProfile kDeviceProfiles[] = {
#define E1000E_DEVICE_PROFILE(device_id, name, flags, pba_kib) \
    {device_id, name, flags, pba_kib},
    E1000E_SUPPORTED_DEVICES(E1000E_DEVICE_PROFILE)
#undef E1000E_DEVICE_PROFILE
};

constexpr bool valid_device_profiles() {
    for (size_t i = 0; i < sizeof(kDeviceProfiles) / sizeof(kDeviceProfiles[0]); ++i) {
        const DeviceProfile& profile = kDeviceProfiles[i];
        if (profile.name == nullptr || profile.pba_kib == 0 ||
            ((profile.flags & DEVICE_I219) != 0 &&
             (profile.flags & DEVICE_PCH_LAN) == 0)) {
            return false;
        }
        for (size_t j = i + 1; j < sizeof(kDeviceProfiles) / sizeof(kDeviceProfiles[0]); ++j) {
            if (profile.device_id == kDeviceProfiles[j].device_id) {
                return false;
            }
        }
    }
    return true;
}

static_assert(valid_device_profiles());

constexpr driver_registry::PciMatch kPciMatches[] = {
#define E1000E_PCI_MATCH_ENTRY(device_id, name, flags, pba_kib) \
    {.vendor = kIntelVendorId, .device = device_id, .class_code = 0x02, .subclass = 0x00, .prog_if = driver_registry::kAnyProgIf},
    E1000E_SUPPORTED_DEVICES(E1000E_PCI_MATCH_ENTRY)
#undef E1000E_PCI_MATCH_ENTRY
};

#undef E1000E_SUPPORTED_DEVICES

constexpr uint8_t kPciCommandIo = 1u << 0;
constexpr uint8_t kPciCommandMemory = 1u << 1;
constexpr uint8_t kPciCommandBusMaster = 1u << 2;
constexpr uint8_t kPciCapabilityPowerManagement = 0x01;
constexpr uint8_t kPciCapabilityPcie = 0x10;
constexpr uint16_t kPciPmcsrPowerStateMask = 0x0003u;
constexpr uint16_t kPcieLinkControlAspmL0s = 0x0001u;
constexpr uint16_t kPcieLinkControlAspmL1 = 0x0002u;

constexpr uint64_t kMmioVirtBase = 0xFFFFE10020000000ull;
constexpr size_t kMmioWindowSize = 2ull * 1024 * 1024;
constexpr size_t kPageSize = 4096;
constexpr size_t kRegisterWindowSize = 128ull * 1024;
constexpr size_t kRingCount = 32;
constexpr size_t kPacketBufferSize = 2048;
constexpr uint32_t kPollSpinCount = 100000;

constexpr uint32_t REG_CTRL = 0x00000;
constexpr uint32_t REG_STATUS = 0x00008;
constexpr uint32_t REG_EECD = 0x00010;
constexpr uint32_t REG_CTRL_EXT = 0x00018;
constexpr uint32_t REG_FEXTNVM7 = 0x000E4;
constexpr uint32_t REG_ICR = 0x000C0;
constexpr uint32_t REG_IMS = 0x000D0;
constexpr uint32_t REG_IMC = 0x000D8;
constexpr uint32_t REG_RCTL = 0x00100;
constexpr uint32_t REG_TCTL = 0x00400;
constexpr uint32_t REG_TIPG = 0x00410;
constexpr uint32_t REG_EXTCNF_CTRL = 0x00F00;
constexpr uint32_t REG_PBA = 0x01000;
constexpr uint32_t REG_PBECCSTS = 0x0100C;
constexpr uint32_t REG_RDBAL0 = 0x02800;
constexpr uint32_t REG_RDBAH0 = 0x02804;
constexpr uint32_t REG_RDLEN0 = 0x02808;
constexpr uint32_t REG_RDH0 = 0x02810;
constexpr uint32_t REG_RDT0 = 0x02818;
constexpr uint32_t REG_RDTR = 0x02820;
constexpr uint32_t REG_RADV = 0x0282C;
constexpr uint32_t REG_RXDCTL0 = 0x02828;
constexpr uint32_t REG_TDBAL0 = 0x03800;
constexpr uint32_t REG_TDBAH0 = 0x03804;
constexpr uint32_t REG_TDLEN0 = 0x03808;
constexpr uint32_t REG_TDH0 = 0x03810;
constexpr uint32_t REG_TDT0 = 0x03818;
constexpr uint32_t REG_TIDV = 0x03820;
constexpr uint32_t REG_TADV = 0x0382C;
constexpr uint32_t REG_TXDCTL0 = 0x03828;
constexpr uint32_t REG_TARC0 = 0x03840;
constexpr uint32_t REG_TARC1 = 0x03940;
constexpr uint32_t REG_KABGTXD = 0x03004;
constexpr uint32_t REG_RFCTL = 0x05008;
constexpr uint32_t REG_RAL0 = 0x05400;
constexpr uint32_t REG_RAH0 = 0x05404;
constexpr uint32_t REG_SWSM = 0x05B50;
constexpr uint32_t REG_FWSM = 0x05B54;

constexpr uint32_t CTRL_SLU = 0x00000040u;
constexpr uint32_t CTRL_ASDE = 0x00000020u;
constexpr uint32_t CTRL_FRCSPD = 0x00000800u;
constexpr uint32_t CTRL_FRCDPX = 0x00001000u;
constexpr uint32_t CTRL_GIO_MASTER_DISABLE = 0x00000004u;
constexpr uint32_t CTRL_RST = 0x04000000u;
constexpr uint32_t CTRL_MEHE = 0x00080000u;
constexpr uint32_t CTRL_PHY_RST = 0x80000000u;
constexpr uint32_t CTRL_EXT_FORCE_SMBUS = 0x00000800u;
constexpr uint32_t CTRL_EXT_RO_DIS = 0x00020000u;
constexpr uint32_t CTRL_EXT_PHYPDEN = 0x00100000u;
constexpr uint32_t CTRL_EXT_REQUIRED_BIT = 0x00400000u;
constexpr uint32_t CTRL_EXT_DRV_LOAD = 0x10000000u;

constexpr uint32_t STATUS_LU = 0x00000002u;
constexpr uint32_t STATUS_LAN_INIT_DONE = 0x00000200u;
constexpr uint32_t STATUS_PHY_RESET_ASSERTED = 0x00000400u;
constexpr uint32_t STATUS_GIO_MASTER_ENABLE = 0x00080000u;

constexpr uint32_t EECD_AUTO_RD = 0x00000200u;
constexpr uint8_t PCI_CONFIG_DESC_RING_STATUS = 0xE4;
constexpr uint16_t DESC_RING_FLUSH_REQUIRED = 0x0100u;
constexpr uint32_t FWSM_PHY_RESET_ALLOWED = 0x00000040u;
constexpr uint32_t FWSM_FW_VALID = 0x00008000u;
constexpr uint32_t EXTCNF_CTRL_SWFLAG = 0x00000020u;

constexpr uint32_t RCTL_EN = 0x00000002u;
constexpr uint32_t RCTL_SBP = 0x00000004u;
constexpr uint32_t RCTL_UPE = 0x00000008u;
constexpr uint32_t RCTL_MPE = 0x00000010u;
constexpr uint32_t RCTL_BAM = 0x00008000u;
constexpr uint32_t RCTL_SECRC = 0x04000000u;
constexpr uint32_t RCTL_BSEX = 0x02000000u;
constexpr uint32_t RCTL_SZ_2048 = 0x00000000u;
constexpr uint32_t RCTL_LBM_NO = 0x00000000u;
constexpr uint32_t RCTL_RDMTS_HALF = 0x00000000u;

constexpr uint32_t RXDCTL_PTHRESH = 0x00000020u;
constexpr uint32_t RXDCTL_HTHRESH = 0x00000100u;
constexpr uint32_t RXDCTL_WTHRESH = 0x00000000u;
constexpr uint32_t RXDCTL_THRESH_UNIT_DESC = 0x01000000u;
constexpr uint32_t RXDCTL_DMA_BURST =
    0x20u | (4u << 8) | (4u << 16) | RXDCTL_THRESH_UNIT_DESC;

constexpr uint32_t TXDCTL_PTHRESH = 0x00000020u;
constexpr uint32_t TXDCTL_HTHRESH = 0x00000100u;
constexpr uint32_t TXDCTL_WTHRESH = 0x00000000u;
constexpr uint32_t TXDCTL_THRESH_UNIT_DESC = 0x01000000u;
constexpr uint32_t TXDCTL_COUNT_DESC = 0x00400000u;
constexpr uint32_t TXDCTL_DMA_BURST =
    0x1Fu | (1u << 8) | (1u << 16) |
    TXDCTL_COUNT_DESC | TXDCTL_THRESH_UNIT_DESC;

constexpr uint32_t TCTL_EN = 0x00000002u;
constexpr uint32_t TCTL_PSP = 0x00000008u;
constexpr uint32_t TCTL_CT = 0x00000FF0u;
constexpr uint32_t TCTL_RTLC = 0x01000000u;
constexpr uint32_t TCTL_COLD = 0x003FF000u;
constexpr uint32_t kCollisionThreshold = 15u;
constexpr uint32_t kCollisionDistance = 63u;
constexpr uint32_t kCtShift = 4u;
constexpr uint32_t kColdShift = 12u;

constexpr uint32_t RAH_AV = 0x80000000u;

constexpr uint32_t TXD_STAT_DD = 0x01u;
constexpr uint8_t TXD_CMD_EOP = 0x01u;
constexpr uint8_t TXD_CMD_IFCS = 0x02u;
constexpr uint8_t TXD_CMD_RS = 0x08u;

constexpr uint32_t RXD_STAT_DD = 0x01u;
constexpr uint32_t RXD_STAT_EOP = 0x02u;
constexpr uint8_t RXD_ERR_FRAME_ERR_MASK = 0x97u;

constexpr uint32_t PBECCSTS_ECC_ENABLE = 0x00010000u;
constexpr uint32_t KABGTXD_BGSQLBIAS = 0x00050000u;
constexpr uint32_t RFCTL_NFSW_DIS = 0x00000040u;
constexpr uint32_t RFCTL_NFSR_DIS = 0x00000080u;
constexpr uint32_t kTipg =
    8u | (8u << 10) | (6u << 20);
constexpr uint32_t kResetSpinCount = 2000000;
constexpr uint32_t kCfgDoneSpinCount = 4000000;
constexpr uint32_t kQuiesceSpinCount = 500000;
constexpr uint32_t kPciD0SettleSpinCount = 2000000;
constexpr uint32_t kResetSettleSpinCount = 2000000;
constexpr uint32_t kPchFwSettleSpinCount = 8000000;
constexpr uint32_t kSwFlagSpinCount = 1000000;
constexpr uint32_t IMS_TXDW = 0x00000001u;
constexpr uint32_t IMS_LSC = 0x00000004u;
constexpr uint32_t IMS_RXDMT0 = 0x00000010u;
constexpr uint32_t IMS_RXO = 0x00000040u;
constexpr uint32_t IMS_RXT0 = 0x00000080u;
constexpr uint32_t kInterruptMask =
    IMS_LSC | IMS_RXDMT0 | IMS_RXO | IMS_RXT0;

struct [[gnu::packed]] RxDescriptor {
    uint64_t buffer_addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t status;
    uint8_t errors;
    uint16_t special;
};

struct [[gnu::packed]] TxDescriptor {
    uint64_t buffer_addr;
    uint16_t length;
    uint8_t cso;
    uint8_t cmd;
    uint8_t status;
    uint8_t css;
    uint16_t special;
};

static_assert(sizeof(RxDescriptor) == 16);
static_assert(sizeof(TxDescriptor) == 16);
static_assert((sizeof(RxDescriptor) * kRingCount) % 128 == 0);

struct DriverState {
    bool initialized;
    bool active;
    bool link_registered;
    bool msi_enabled;
    bool poll_registered;
    bool pch_lan;
    bool i219;
    bool dma_burst;
    uint8_t interrupt_vector;
    uint8_t pba_kib;
    const char* device_name;
    pci::PciDevice device;
    volatile uint8_t* regs;
    uint8_t mac[6];
    size_t rx_index;
    size_t tx_head;
    size_t tx_tail;
    RxDescriptor* rx_ring;
    TxDescriptor* tx_ring;
    uint64_t rx_ring_phys;
    uint64_t tx_ring_phys;
    uint64_t rx_buffer_phys[kRingCount];
    uint64_t tx_buffer_phys[kRingCount];
    uint8_t* rx_buffer_virt[kRingCount];
    uint8_t* tx_buffer_virt[kRingCount];
    uint32_t tx_submitted;
    uint32_t tx_completed;
    uint32_t rx_desc_seen;
    uint32_t rx_frames_passed;
    net::LinkDevice link_device;
};

DriverState g_state{};
uint64_t g_mmio_next_virt = kMmioVirtBase;
sync::SpinLock g_tx_lock;

void handle_interrupt();

inline uint32_t mmio_read32(uint32_t offset) {
    return *reinterpret_cast<volatile uint32_t*>(
        const_cast<volatile uint8_t*>(g_state.regs) + offset);
}

inline void mmio_write32(uint32_t offset, uint32_t value) {
    *reinterpret_cast<volatile uint32_t*>(
        const_cast<volatile uint8_t*>(g_state.regs) + offset) = value;
}

void mmio_flush() {
    (void)mmio_read32(REG_STATUS);
}

void cpu_relax() {
    asm volatile("pause");
}

const DeviceProfile* find_device_profile(uint16_t device_id) {
    for (size_t i = 0; i < sizeof(kDeviceProfiles) / sizeof(kDeviceProfiles[0]); ++i) {
        if (kDeviceProfiles[i].device_id == device_id) {
            return &kDeviceProfiles[i];
        }
    }
    return nullptr;
}

uint64_t align_down_u64(uint64_t value, uint64_t alignment) {
    return value & ~(alignment - 1);
}

uint64_t align_up_u64(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
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
        out[i * 3] = hex_digit(static_cast<uint8_t>((byte >> 4) & 0x0Fu));
        out[i * 3 + 1] = hex_digit(static_cast<uint8_t>(byte & 0x0Fu));
        if (i != 5) {
            out[i * 3 + 2] = ':';
        }
    }
    out[17] = '\0';
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

volatile uint8_t* map_bar_region(const pci::PciDevice& device, uint8_t bar_index) {
    uint64_t bar_base = pci_bar_base(device, bar_index);
    if (bar_base == 0) {
        return nullptr;
    }

    uint64_t page_phys = align_down_u64(bar_base, kPageSize);
    uint64_t page_end = align_up_u64(bar_base + kRegisterWindowSize, kPageSize);
    size_t page_count = static_cast<size_t>((page_end - page_phys) / kPageSize);
    if (page_count == 0) {
        page_count = 1;
    }

    uint64_t virt_base = g_mmio_next_virt;
    uint64_t virt_end = virt_base + static_cast<uint64_t>(page_count) * kPageSize;
    if (virt_end - kMmioVirtBase > kMmioWindowSize) {
        log_message(LogLevel::Warn,
                    "e1000e: MMIO window exhausted while mapping BAR%u",
                    static_cast<unsigned int>(bar_index));
        return nullptr;
    }

    const uint64_t mmio_flags = PAGE_FLAG_WRITE | PAGE_FLAG_WRITE_THROUGH |
                                PAGE_FLAG_CACHE_DISABLE | PAGE_FLAG_NO_EXECUTE;
    for (size_t i = 0; i < page_count; ++i) {
        uint64_t phys = page_phys + static_cast<uint64_t>(i) * kPageSize;
        uint64_t virt = virt_base + static_cast<uint64_t>(i) * kPageSize;
        if (!paging_map_page(virt, phys, mmio_flags)) {
            log_message(LogLevel::Warn,
                        "e1000e: failed to map MMIO page phys=%016llx",
                        static_cast<unsigned long long>(phys));
            return nullptr;
        }
    }

    g_mmio_next_virt = virt_end;
    return reinterpret_cast<volatile uint8_t*>(virt_base + (bar_base - page_phys));
}

bool wait_for_mask_spins(uint32_t reg, uint32_t mask, uint32_t expected, uint32_t spins) {
    for (uint32_t i = 0; i < spins; ++i) {
        if ((mmio_read32(reg) & mask) == expected) {
            return true;
        }
        cpu_relax();
    }
    return false;
}

void spin_delay(uint32_t spins) {
    for (uint32_t i = 0; i < spins; ++i) {
        cpu_relax();
    }
}

void dma_write_barrier() {
    asm volatile("sfence" ::: "memory");
}

void prepare_pci_power_and_link(const pci::PciDevice& device,
                                const DeviceProfile& profile) {
    uint8_t pm_cap = pci::find_capability(device, kPciCapabilityPowerManagement);
    if (pm_cap != 0) {
        uint8_t pmcsr_offset = static_cast<uint8_t>(pm_cap + 4);
        uint16_t pmcsr = pci::read_config16(device, pmcsr_offset);
        uint16_t power_state = pmcsr & kPciPmcsrPowerStateMask;
        if (power_state != 0) {
            pci::write_config16(
                device,
                pmcsr_offset,
                static_cast<uint16_t>(pmcsr & ~kPciPmcsrPowerStateMask));
            spin_delay(kPciD0SettleSpinCount);
            log_message(LogLevel::Info,
                        "e1000e: powered %02u:%02u.%u from D%u to D0",
                        static_cast<unsigned int>(device.bus),
                        static_cast<unsigned int>(device.slot),
                        static_cast<unsigned int>(device.function),
                        static_cast<unsigned int>(power_state));
        }
    }

    uint16_t disable_aspm = 0;
    if ((profile.flags & DEVICE_DISABLE_ASPM_L0S) != 0) {
        disable_aspm |= kPcieLinkControlAspmL0s;
    }
    if ((profile.flags & DEVICE_DISABLE_ASPM_L1) != 0) {
        disable_aspm |= kPcieLinkControlAspmL1;
    }
    uint8_t pcie_cap = pci::find_capability(device, kPciCapabilityPcie);
    if (pcie_cap != 0 && disable_aspm != 0) {
        uint8_t link_control_offset = static_cast<uint8_t>(pcie_cap + 0x10);
        uint16_t link_control = pci::read_config16(device, link_control_offset);
        pci::write_config16(device,
                            link_control_offset,
                            static_cast<uint16_t>(link_control & ~disable_aspm));
    }
}

bool wait_for_cfg_done() {
    if (wait_for_mask_spins(REG_STATUS,
                            STATUS_LAN_INIT_DONE,
                            STATUS_LAN_INIT_DONE,
                            kCfgDoneSpinCount)) {
        return true;
    }
    return wait_for_mask_spins(REG_EECD,
                               EECD_AUTO_RD,
                               EECD_AUTO_RD,
                               kCfgDoneSpinCount);
}

bool verbose_stage_logs(const pci::PciDevice& device) {
    return device.vendor == kIntelVendorId &&
           (device.device == 0x155Au || device.device == 0x156Fu || device.device == 0x1570u);
}

void log_init_stage(const pci::PciDevice& device, const char* stage) {
    if (!verbose_stage_logs(device) || stage == nullptr) {
        return;
    }
    log_message(LogLevel::Info,
                "e1000e: %02u:%02u.%u dev=%04x stage=%s",
                static_cast<unsigned int>(device.bus),
                static_cast<unsigned int>(device.slot),
                static_cast<unsigned int>(device.function),
                static_cast<unsigned int>(device.device),
                stage);
}

void log_register_snapshot(const pci::PciDevice& device, const char* stage) {
    if (!verbose_stage_logs(device) || stage == nullptr) {
        return;
    }
    log_message(LogLevel::Info,
                "e1000e: %02u:%02u.%u stage=%s ctrl=%08x status=%08x ctrl_ext=%08x eecd=%08x "
                "fextnvm7=%08x swsm=%08x fwsm=%08x",
                static_cast<unsigned int>(device.bus),
                static_cast<unsigned int>(device.slot),
                static_cast<unsigned int>(device.function),
                stage,
                static_cast<unsigned int>(mmio_read32(REG_CTRL)),
                static_cast<unsigned int>(mmio_read32(REG_STATUS)),
                static_cast<unsigned int>(mmio_read32(REG_CTRL_EXT)),
                static_cast<unsigned int>(mmio_read32(REG_EECD)),
                static_cast<unsigned int>(mmio_read32(REG_FEXTNVM7)),
                static_cast<unsigned int>(mmio_read32(REG_SWSM)),
                static_cast<unsigned int>(mmio_read32(REG_FWSM)));
}

void clear_force_smbus_if_needed(const pci::PciDevice& device, bool pch_lan) {
    uint32_t ctrl_ext = mmio_read32(REG_CTRL_EXT);
    if ((ctrl_ext & CTRL_EXT_FORCE_SMBUS) == 0) {
        return;
    }

    // On managed PCH parts, the management engine owns the PHY interface.
    // Switching it away from SMBus while firmware is active can break AMT.
    if (pch_lan && (mmio_read32(REG_FWSM) & FWSM_FW_VALID) != 0) {
        log_message(LogLevel::Info,
                    "e1000e: preserving firmware-owned SMBus mode on %02u:%02u.%u",
                    static_cast<unsigned int>(device.bus),
                    static_cast<unsigned int>(device.slot),
                    static_cast<unsigned int>(device.function));
        return;
    }

    mmio_write32(REG_CTRL_EXT, ctrl_ext & ~CTRL_EXT_FORCE_SMBUS);
    mmio_flush();
    log_message(LogLevel::Info,
                "e1000e: cleared CTRL_EXT.FORCE_SMBUS on %02u:%02u.%u device=%04x",
                static_cast<unsigned int>(device.bus),
                static_cast<unsigned int>(device.slot),
                static_cast<unsigned int>(device.function),
                static_cast<unsigned int>(device.device));
}

bool acquire_pch_swflag() {
    if (!wait_for_mask_spins(REG_EXTCNF_CTRL,
                             EXTCNF_CTRL_SWFLAG,
                             0,
                             kSwFlagSpinCount)) {
        return false;
    }

    uint32_t extcnf_ctrl = mmio_read32(REG_EXTCNF_CTRL);
    mmio_write32(REG_EXTCNF_CTRL, extcnf_ctrl | EXTCNF_CTRL_SWFLAG);
    mmio_flush();
    return wait_for_mask_spins(REG_EXTCNF_CTRL,
                               EXTCNF_CTRL_SWFLAG,
                               EXTCNF_CTRL_SWFLAG,
                               kSwFlagSpinCount);
}

void release_pch_swflag() {
    uint32_t extcnf_ctrl = mmio_read32(REG_EXTCNF_CTRL);
    if ((extcnf_ctrl & EXTCNF_CTRL_SWFLAG) != 0) {
        mmio_write32(REG_EXTCNF_CTRL, extcnf_ctrl & ~EXTCNF_CTRL_SWFLAG);
        mmio_flush();
    }
}

void initialize_pch_hardware_bits() {
    uint32_t ctrl_ext = mmio_read32(REG_CTRL_EXT);
    ctrl_ext |= CTRL_EXT_REQUIRED_BIT | CTRL_EXT_PHYPDEN |
                CTRL_EXT_RO_DIS | CTRL_EXT_DRV_LOAD;
    mmio_write32(REG_CTRL_EXT, ctrl_ext);

    uint32_t tarc0 = mmio_read32(REG_TARC0);
    tarc0 |= (1u << 23) | (1u << 24) | (1u << 26) | (1u << 27);
    mmio_write32(REG_TARC0, tarc0);

    uint32_t tarc1 = mmio_read32(REG_TARC1);
    tarc1 |= (1u << 24) | (1u << 26) | (1u << 28) | (1u << 30);
    mmio_write32(REG_TARC1, tarc1);

    mmio_write32(REG_RFCTL,
                 mmio_read32(REG_RFCTL) | RFCTL_NFSW_DIS | RFCTL_NFSR_DIS);
    mmio_write32(REG_PBECCSTS,
                 mmio_read32(REG_PBECCSTS) | PBECCSTS_ECC_ENABLE);
    mmio_write32(REG_KABGTXD,
                 mmio_read32(REG_KABGTXD) | KABGTXD_BGSQLBIAS);
    mmio_write32(REG_CTRL, mmio_read32(REG_CTRL) | CTRL_MEHE);
    mmio_flush();
}

bool disable_pcie_master_requests() {
    uint32_t ctrl = mmio_read32(REG_CTRL);
    mmio_write32(REG_CTRL, ctrl | CTRL_GIO_MASTER_DISABLE);
    mmio_flush();
    return wait_for_mask_spins(REG_STATUS,
                               STATUS_GIO_MASTER_ENABLE,
                               0,
                               kResetSpinCount);
}

void restore_master_requesting() {
    uint32_t ctrl = mmio_read32(REG_CTRL);
    if ((ctrl & CTRL_GIO_MASTER_DISABLE) == 0) {
        return;
    }
    mmio_write32(REG_CTRL, ctrl & ~CTRL_GIO_MASTER_DISABLE);
    mmio_flush();
}

bool quiesce_dma_for_release(const pci::PciDevice& device) {
    mmio_write32(REG_IMC, 0xFFFFFFFFu);
    mmio_write32(REG_RCTL, 0);
    mmio_write32(REG_TCTL, 0);
    mmio_flush();

    bool requests_drained = disable_pcie_master_requests();
    uint16_t command = pci::read_config16(device, 0x04);
    command &= static_cast<uint16_t>(~kPciCommandBusMaster);
    pci::write_config16(device, 0x04, command);
    spin_delay(kQuiesceSpinCount);
    bool bus_master_disabled =
        (pci::read_config16(device, 0x04) & kPciCommandBusMaster) == 0;
    return requests_drained && bus_master_disabled;
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

size_t ring_page_count() {
    return (sizeof(RxDescriptor) * kRingCount + kPageSize - 1) / kPageSize;
}

void release_dma_allocations() {
    for (size_t i = 0; i < kRingCount; ++i) {
        if (g_state.rx_buffer_phys[i] != 0) {
            memory::free_kernel_page(g_state.rx_buffer_phys[i]);
            g_state.rx_buffer_phys[i] = 0;
            g_state.rx_buffer_virt[i] = nullptr;
        }
        if (g_state.tx_buffer_phys[i] != 0) {
            memory::free_kernel_page(g_state.tx_buffer_phys[i]);
            g_state.tx_buffer_phys[i] = 0;
            g_state.tx_buffer_virt[i] = nullptr;
        }
    }
    if (g_state.rx_ring_phys != 0) {
        memory::free_kernel_block(g_state.rx_ring_phys);
        g_state.rx_ring_phys = 0;
        g_state.rx_ring = nullptr;
    }
    if (g_state.tx_ring_phys != 0) {
        memory::free_kernel_block(g_state.tx_ring_phys);
        g_state.tx_ring_phys = 0;
        g_state.tx_ring = nullptr;
    }
}

bool load_mac_address(uint8_t mac[6]) {
    uint32_t ral = mmio_read32(REG_RAL0);
    uint32_t rah = mmio_read32(REG_RAH0);
    if ((rah & RAH_AV) == 0 && (ral == 0 || ral == 0xFFFFFFFFu)) {
        return false;
    }

    mac[0] = static_cast<uint8_t>(ral & 0xFFu);
    mac[1] = static_cast<uint8_t>((ral >> 8) & 0xFFu);
    mac[2] = static_cast<uint8_t>((ral >> 16) & 0xFFu);
    mac[3] = static_cast<uint8_t>((ral >> 24) & 0xFFu);
    mac[4] = static_cast<uint8_t>(rah & 0xFFu);
    mac[5] = static_cast<uint8_t>((rah >> 8) & 0xFFu);

    // Reject multicast, broadcast, and all-zero addresses.  A set RAH.AV bit
    // is not sufficient evidence that firmware/NVM supplied a usable address.
    if ((mac[0] & 0x01u) != 0) {
        return false;
    }
    bool any_nonzero = false;
    bool any_not_ff = false;
    for (size_t i = 0; i < 6; ++i) {
        any_nonzero = any_nonzero || mac[i] != 0;
        any_not_ff = any_not_ff || mac[i] != 0xFFu;
    }
    return any_nonzero && any_not_ff;
}

void program_mac_address(const uint8_t mac[6]) {
    uint32_t ral = static_cast<uint32_t>(mac[0]) |
                   (static_cast<uint32_t>(mac[1]) << 8) |
                   (static_cast<uint32_t>(mac[2]) << 16) |
                   (static_cast<uint32_t>(mac[3]) << 24);
    uint32_t rah = static_cast<uint32_t>(mac[4]) |
                   (static_cast<uint32_t>(mac[5]) << 8) |
                   RAH_AV;
    mmio_write32(REG_RAL0, ral);
    mmio_write32(REG_RAH0, rah);
}

bool setup_rx_ring() {
    g_state.rx_ring_phys = alloc_zeroed_pages(ring_page_count());
    if (g_state.rx_ring_phys == 0) {
        log_message(LogLevel::Warn, "e1000e: failed to allocate RX descriptor ring");
        return false;
    }

    g_state.rx_ring = static_cast<RxDescriptor*>(paging_phys_to_virt(g_state.rx_ring_phys));
    if (g_state.rx_ring == nullptr) {
        log_message(LogLevel::Warn, "e1000e: failed to map RX descriptor ring");
        return false;
    }

    for (size_t i = 0; i < kRingCount; ++i) {
        g_state.rx_buffer_phys[i] = memory::alloc_kernel_page();
        if (g_state.rx_buffer_phys[i] == 0) {
            log_message(LogLevel::Warn,
                        "e1000e: failed to allocate RX buffer %zu/%zu",
                        i,
                        kRingCount);
            return false;
        }
        g_state.rx_buffer_virt[i] =
            static_cast<uint8_t*>(paging_phys_to_virt(g_state.rx_buffer_phys[i]));
        if (g_state.rx_buffer_virt[i] == nullptr) {
            log_message(LogLevel::Warn,
                        "e1000e: failed to map RX buffer %zu/%zu",
                        i,
                        kRingCount);
            return false;
        }
        memset(g_state.rx_buffer_virt[i], 0, kPacketBufferSize);
        g_state.rx_ring[i].buffer_addr = g_state.rx_buffer_phys[i];
        g_state.rx_ring[i].length = 0;
        g_state.rx_ring[i].checksum = 0;
        g_state.rx_ring[i].status = 0;
        g_state.rx_ring[i].errors = 0;
        g_state.rx_ring[i].special = 0;
    }

    g_state.rx_index = 0;

    mmio_write32(REG_RCTL, 0);
    mmio_flush();
    mmio_write32(REG_RDTR, 0);
    mmio_write32(REG_RADV, 0);
    uint32_t rxdctl = g_state.dma_burst
                          ? RXDCTL_DMA_BURST
                          : RXDCTL_PTHRESH | RXDCTL_HTHRESH |
                                RXDCTL_WTHRESH | RXDCTL_THRESH_UNIT_DESC;
    mmio_write32(REG_RXDCTL0, rxdctl);
    mmio_write32(REG_RDBAL0, static_cast<uint32_t>(g_state.rx_ring_phys & 0xFFFFFFFFu));
    mmio_write32(REG_RDBAH0, static_cast<uint32_t>(g_state.rx_ring_phys >> 32));
    mmio_write32(REG_RDLEN0, static_cast<uint32_t>(sizeof(RxDescriptor) * kRingCount));
    mmio_write32(REG_RDH0, 0);
    dma_write_barrier();
    mmio_write32(REG_RDT0, static_cast<uint32_t>(kRingCount - 1));

    uint32_t rctl = RCTL_EN | RCTL_BAM | RCTL_SECRC | RCTL_LBM_NO |
                    RCTL_RDMTS_HALF | RCTL_SZ_2048;
    rctl &= ~RCTL_BSEX;
    mmio_write32(REG_RCTL, rctl);
    mmio_flush();
    return true;
}

bool setup_tx_ring() {
    g_state.tx_ring_phys = alloc_zeroed_pages(ring_page_count());
    if (g_state.tx_ring_phys == 0) {
        log_message(LogLevel::Warn, "e1000e: failed to allocate TX descriptor ring");
        return false;
    }

    g_state.tx_ring = static_cast<TxDescriptor*>(paging_phys_to_virt(g_state.tx_ring_phys));
    if (g_state.tx_ring == nullptr) {
        log_message(LogLevel::Warn, "e1000e: failed to map TX descriptor ring");
        return false;
    }

    for (size_t i = 0; i < kRingCount; ++i) {
        g_state.tx_buffer_phys[i] = memory::alloc_kernel_page();
        if (g_state.tx_buffer_phys[i] == 0) {
            log_message(LogLevel::Warn,
                        "e1000e: failed to allocate TX buffer %zu/%zu",
                        i,
                        kRingCount);
            return false;
        }
        g_state.tx_buffer_virt[i] =
            static_cast<uint8_t*>(paging_phys_to_virt(g_state.tx_buffer_phys[i]));
        if (g_state.tx_buffer_virt[i] == nullptr) {
            log_message(LogLevel::Warn,
                        "e1000e: failed to map TX buffer %zu/%zu",
                        i,
                        kRingCount);
            return false;
        }
        memset(g_state.tx_buffer_virt[i], 0, kPacketBufferSize);
        g_state.tx_ring[i].buffer_addr = g_state.tx_buffer_phys[i];
        g_state.tx_ring[i].status = TXD_STAT_DD;
    }

    g_state.tx_head = 0;
    g_state.tx_tail = 0;

    mmio_write32(REG_TIDV, 0);
    mmio_write32(REG_TADV, 0);
    uint32_t txdctl = g_state.dma_burst
                          ? TXDCTL_DMA_BURST
                          : TXDCTL_PTHRESH | TXDCTL_HTHRESH |
                                TXDCTL_WTHRESH | TXDCTL_THRESH_UNIT_DESC;
    mmio_write32(REG_TXDCTL0, txdctl);
    mmio_write32(REG_TDBAL0, static_cast<uint32_t>(g_state.tx_ring_phys & 0xFFFFFFFFu));
    mmio_write32(REG_TDBAH0, static_cast<uint32_t>(g_state.tx_ring_phys >> 32));
    mmio_write32(REG_TDLEN0, static_cast<uint32_t>(sizeof(TxDescriptor) * kRingCount));
    mmio_write32(REG_TDH0, 0);
    mmio_write32(REG_TDT0, 0);
    mmio_write32(REG_TIPG, kTipg);

    uint32_t tctl = TCTL_PSP | TCTL_RTLC |
                    (kCollisionThreshold << kCtShift) |
                    (kCollisionDistance << kColdShift);
    tctl |= TCTL_EN;
    mmio_write32(REG_TCTL, tctl);
    mmio_flush();
    return true;
}

void update_link_state() {
    bool up = (mmio_read32(REG_STATUS) & STATUS_LU) != 0;
    g_state.link_device.up = up;
}

bool init_device(const pci::PciDevice& device) {
    const DeviceProfile* profile = find_device_profile(device.device);
    if (profile == nullptr) {
        return false;
    }

    // DriverState contains the network link queues and is much larger than a
    // kernel stack.  Initialize the module's persistent instance directly;
    // placing a temporary DriverState here overflows the bootstrap stack.
    memset(&g_state, 0, sizeof(g_state));
    g_state.initialized = true;
    g_state.device = device;
    g_state.pch_lan = (profile->flags & DEVICE_PCH_LAN) != 0;
    g_state.i219 = (profile->flags & DEVICE_I219) != 0;
    g_state.dma_burst = (profile->flags & DEVICE_DMA_BURST) != 0;
    g_state.pba_kib = profile->pba_kib;
    g_state.device_name = profile->name;

    log_init_stage(device, "pci-enable");

    prepare_pci_power_and_link(device, *profile);

    uint16_t command = pci::read_config16(device, 0x04);
    command |= kPciCommandMemory;
    command &= static_cast<uint16_t>(~(kPciCommandIo | kPciCommandBusMaster));
    pci::write_config16(device, 0x04, command);

    log_init_stage(device, "map-bar0");
    g_state.regs = map_bar_region(device, 0);
    if (g_state.regs == nullptr) {
        log_message(LogLevel::Warn,
                    "e1000e: failed to map BAR0 for %02u:%02u.%u",
                    static_cast<unsigned int>(device.bus),
                    static_cast<unsigned int>(device.slot),
                    static_cast<unsigned int>(device.function));
        return false;
    }

    log_register_snapshot(device, "mapped");

    log_init_stage(device, "mask-interrupts");
    mmio_write32(REG_IMC, 0xFFFFFFFFu);
    (void)mmio_read32(REG_ICR);

    // Quiesce the device before reset. PCH LAN parts can wedge the PCIe bus
    // if a full MAC reset is issued while bus mastering is still active.
    log_init_stage(device, "quiesce");
    mmio_write32(REG_RCTL, 0);
    mmio_write32(REG_TCTL, TCTL_PSP);
    mmio_flush();
    spin_delay(kQuiesceSpinCount);
    log_register_snapshot(device, "quiesced");

    log_init_stage(device, "clear-force-smbus");
    if (!g_state.pch_lan) {
        clear_force_smbus_if_needed(device, false);
    }

    log_init_stage(device, "disable-pcie-master");
    if (!disable_pcie_master_requests()) {
        log_message(LogLevel::Warn,
                    "e1000e: pending PCIe master requests at %02u:%02u.%u device=%04x",
                    static_cast<unsigned int>(device.bus),
                    static_cast<unsigned int>(device.slot),
                    static_cast<unsigned int>(device.function),
                    static_cast<unsigned int>(device.device));
    }

    log_init_stage(device, "disable-bus-master");
    command = pci::read_config16(device, 0x04);
    command &= static_cast<uint16_t>(~kPciCommandBusMaster);
    pci::write_config16(device, 0x04, command);
    spin_delay(kQuiesceSpinCount);
    log_register_snapshot(device, "bus-master-disabled");

    uint32_t ctrl = mmio_read32(REG_CTRL);
    // PCH LAN reset requires generation-specific PHY/ULP/K1 and clock
    // workarounds that this driver does not yet implement. Preserve firmware
    // initialization instead of risking link loss or broken DMA.
    bool skip_hard_reset = g_state.pch_lan;
    bool pch_swflag_acquired = false;

    // I219 reports the descriptor-ring hang workaround in PCI configuration
    // space, not in the same-numbered FEXTNVM7 MMIO register. Resetting while
    // this bit is set can wedge the MAC until the next PCI function reset.
    if (g_state.i219 &&
        (pci::read_config16(device, PCI_CONFIG_DESC_RING_STATUS) &
         DESC_RING_FLUSH_REQUIRED) != 0) {
        log_message(LogLevel::Error,
                    "e1000e: I219 descriptor flush required at %02u:%02u.%u; "
                    "leaving the controller quiesced",
                    static_cast<unsigned int>(device.bus),
                    static_cast<unsigned int>(device.slot),
                    static_cast<unsigned int>(device.function));
        return false;
    }

    if (g_state.pch_lan && !skip_hard_reset) {
        pch_swflag_acquired = acquire_pch_swflag();
        if (!pch_swflag_acquired) {
            log_message(LogLevel::Warn,
                        "e1000e: firmware did not release the PCH shared resource at "
                        "%02u:%02u.%u; preserving hardware state",
                        static_cast<unsigned int>(device.bus),
                        static_cast<unsigned int>(device.slot),
                        static_cast<unsigned int>(device.function));
            skip_hard_reset = true;
        } else {
            clear_force_smbus_if_needed(device, true);
        }
    }

    if (!skip_hard_reset) {
        log_init_stage(device, "issue-reset");
        if (g_state.pch_lan &&
            (mmio_read32(REG_FWSM) & FWSM_PHY_RESET_ALLOWED) != 0) {
            ctrl |= CTRL_PHY_RST;
        }
        mmio_write32(REG_CTRL, ctrl | CTRL_RST);
        // Let the MAC settle before polling registers after a global reset.
        spin_delay(kResetSettleSpinCount);
        if (!wait_for_mask_spins(REG_CTRL, CTRL_RST, 0, kResetSpinCount)) {
            log_message(LogLevel::Warn,
                        "e1000e: controller reset timed out at %02u:%02u.%u",
                        static_cast<unsigned int>(device.bus),
                        static_cast<unsigned int>(device.slot),
                        static_cast<unsigned int>(device.function));
        }

        log_init_stage(device, "wait-config");
        if (!wait_for_cfg_done()) {
            log_message(LogLevel::Warn,
                        "e1000e: config done timed out at %02u:%02u.%u",
                        static_cast<unsigned int>(device.bus),
                        static_cast<unsigned int>(device.slot),
                        static_cast<unsigned int>(device.function));
        }
        uint32_t status = mmio_read32(REG_STATUS);
        if ((status & STATUS_PHY_RESET_ASSERTED) != 0) {
            mmio_write32(REG_STATUS, status & ~STATUS_PHY_RESET_ASSERTED);
        }
        if (pch_swflag_acquired) {
            release_pch_swflag();
        }
    } else {
        log_init_stage(device, "pch-fw-settle");
        spin_delay(kPchFwSettleSpinCount);
        (void)wait_for_cfg_done();
    }

    log_init_stage(device, "post-reset-init");
    ctrl = mmio_read32(REG_CTRL);
    ctrl |= CTRL_SLU | CTRL_ASDE;
    ctrl &= ~(CTRL_FRCSPD | CTRL_FRCDPX);
    mmio_write32(REG_CTRL, ctrl);
    if (g_state.pch_lan) {
        initialize_pch_hardware_bits();
    } else {
        mmio_write32(REG_CTRL_EXT, mmio_read32(REG_CTRL_EXT) | CTRL_EXT_DRV_LOAD);
    }
    mmio_write32(REG_PBA, g_state.pba_kib);
    mmio_write32(REG_IMC, 0xFFFFFFFFu);
    (void)mmio_read32(REG_ICR);
    log_register_snapshot(device, "post-reset-init");

    log_init_stage(device, "read-mac");
    if (!load_mac_address(g_state.mac)) {
        log_message(LogLevel::Warn,
                    "e1000e: failed to read MAC address at %02u:%02u.%u",
                    static_cast<unsigned int>(device.bus),
                    static_cast<unsigned int>(device.slot),
                    static_cast<unsigned int>(device.function));
        return false;
    }
    program_mac_address(g_state.mac);

    log_init_stage(device, "reenable-bus-master");
    command = pci::read_config16(device, 0x04);
    command |= static_cast<uint16_t>(kPciCommandMemory | kPciCommandBusMaster);
    command &= static_cast<uint16_t>(~kPciCommandIo);
    pci::write_config16(device, 0x04, command);
    restore_master_requesting();

    log_init_stage(device, "setup-rings");
    if (!setup_rx_ring() || !setup_tx_ring()) {
        log_message(LogLevel::Warn, "e1000e: failed to allocate descriptor rings");
        if (quiesce_dma_for_release(device)) {
            release_dma_allocations();
        } else {
            // The controller may still own a descriptor. Leaking these pages
            // is safer than allowing late DMA into recycled kernel memory.
            log_message(LogLevel::Error,
                        "e1000e: retaining DMA pages after quiesce timeout");
        }
        return false;
    }

    log_init_stage(device, "register-link");
    update_link_state();

    g_state.link_registered =
        net::register_link(g_state.link_device, "e1000e", &g_state, transmit_frame, g_state.mac);
    if (!g_state.link_registered) {
        log_message(LogLevel::Warn, "e1000e: failed to register link device");
        if (quiesce_dma_for_release(device)) {
            release_dma_allocations();
        } else {
            log_message(LogLevel::Error,
                        "e1000e: retaining DMA pages after quiesce timeout");
        }
        return false;
    }

    g_state.active = true;
    g_state.msi_enabled = false;
    g_state.interrupt_vector = 0;

    uint8_t vector = interrupts::allocate_vector();
    if (vector != 0 &&
        interrupts::register_vector(vector, handle_interrupt) &&
        pci::enable_msi(device,
                        vector,
                        static_cast<uint8_t>(lapic::id()))) {
        g_state.msi_enabled = true;
        g_state.interrupt_vector = vector;
        mmio_write32(REG_IMS, kInterruptMask);
        (void)mmio_read32(REG_ICR);
        log_message(LogLevel::Info, "e1000e: using MSI vector %u",
                    static_cast<unsigned int>(vector));
    } else {
        if (vector != 0) {
            interrupts::unregister_vector(vector);
        }
    }

    g_state.poll_registered = scheduler::register_poll(poll);
    if (!g_state.poll_registered && !g_state.msi_enabled) {
        log_message(LogLevel::Warn,
                    "e1000e: failed to register MSI or deferred poll");
    }

    char mac_string[18];
    format_mac_string(g_state.mac, mac_string, sizeof(mac_string));
    log_message(LogLevel::Info,
                "e1000e: online at %02u:%02u.%u device=%04x (%s) mac=%s link=%s",
                static_cast<unsigned int>(device.bus),
                static_cast<unsigned int>(device.slot),
                static_cast<unsigned int>(device.function),
                static_cast<unsigned int>(device.device),
                g_state.device_name,
                mac_string,
                g_state.link_device.up ? "up" : "down");
    return true;
}

void reap_tx_locked() {
    while (g_state.tx_head != g_state.tx_tail) {
        TxDescriptor& desc = g_state.tx_ring[g_state.tx_head];
        if ((__atomic_load_n(&desc.status, __ATOMIC_ACQUIRE) & TXD_STAT_DD) == 0) {
            break;
        }
        ++g_state.tx_completed;
        ++g_state.tx_head;
        if (g_state.tx_head >= kRingCount) {
            g_state.tx_head = 0;
        }
    }
}

void reap_tx() {
    sync::IrqLockGuard guard(g_tx_lock);
    reap_tx_locked();
}

void recycle_rx_descriptor(size_t index) {
    RxDescriptor& desc = g_state.rx_ring[index];
    desc.buffer_addr = g_state.rx_buffer_phys[index];
    desc.length = 0;
    desc.checksum = 0;
    desc.status = 0;
    desc.errors = 0;
    desc.special = 0;
    dma_write_barrier();
    mmio_write32(REG_RDT0, static_cast<uint32_t>(index));
}

void service_device() {
    update_link_state();
    reap_tx();

    for (;;) {
        RxDescriptor& desc = g_state.rx_ring[g_state.rx_index];
        uint8_t status = __atomic_load_n(&desc.status, __ATOMIC_ACQUIRE);
        uint8_t errors = desc.errors;
        if ((status & RXD_STAT_DD) == 0) {
            break;
        }
        ++g_state.rx_desc_seen;

        size_t length = desc.length;
        if ((status & RXD_STAT_EOP) != 0 &&
            (errors & RXD_ERR_FRAME_ERR_MASK) == 0 &&
            length != 0 &&
            length <= kPacketBufferSize) {
            ++g_state.rx_frames_passed;
            net::receive_frame(&g_state.link_device,
                               g_state.rx_buffer_virt[g_state.rx_index],
                               length);
        } else if (errors != 0) {
            log_message(LogLevel::Debug,
                        "e1000e: dropped RX frame status=%02x err=%02x len=%u",
                        static_cast<unsigned int>(status),
                        static_cast<unsigned int>(errors),
                        static_cast<unsigned int>(length));
        }

        recycle_rx_descriptor(g_state.rx_index);
        ++g_state.rx_index;
        if (g_state.rx_index >= kRingCount) {
            g_state.rx_index = 0;
        }
    }
}

void handle_interrupt() {
    if (!g_state.active || !g_state.msi_enabled) {
        return;
    }
    if (mmio_read32(REG_ICR) == 0) {
        return;
    }
    if (!g_state.poll_registered) {
        service_device();
    }
}

}  // namespace

void register_driver() {
#ifdef NEUTRINO_DYNAMIC_MODULE_E1000E
    if (g_module_api == nullptr || g_module_api->register_pci_driver == nullptr) {
        return;
    }
    (void)g_module_api->register_pci_driver(
        "e1000e",
        kPciMatches,
        sizeof(kPciMatches) / sizeof(kPciMatches[0]),
        init);
#else
    (void)driver_registry::register_pci_driver(
        "e1000e",
        kPciMatches,
        sizeof(kPciMatches) / sizeof(kPciMatches[0]),
        init);
#endif
}

bool register_module() {
    register_driver();
    return true;
}

#ifndef NEUTRINO_DYNAMIC_MODULE_E1000E
KERNEL_BUILTIN_MODULE(e1000e_module,
                      "e1000e",
                      kernel_module::Phase::Driver,
                      register_module,
                      kPciMatches,
                      sizeof(kPciMatches) / sizeof(kPciMatches[0]));
#else
extern "C" bool neutrino_module_init(const kernel_module::Api* api) {
    if (api == nullptr ||
        api->abi_version != kernel_module::kDescriptorAbiVersion) {
        return false;
    }
    g_module_api = api;
    register_driver();
    return true;
}
#endif

void init() {
    if (g_state.initialized) {
        return;
    }

    g_state.initialized = true;

    const pci::PciDevice* list = pci::devices();
    size_t count = pci::device_count();
    for (size_t i = 0; i < count; ++i) {
        const pci::PciDevice& dev = list[i];
        if (dev.vendor != kIntelVendorId ||
            dev.class_code != 0x02 ||
            dev.subclass != 0x00) {
            continue;
        }

        if (find_device_profile(dev.device) == nullptr) {
            log_message(LogLevel::Info,
                        "e1000e: skipping unsupported Intel ethernet device %04x at %02u:%02u.%u",
                        static_cast<unsigned int>(dev.device),
                        static_cast<unsigned int>(dev.bus),
                        static_cast<unsigned int>(dev.slot),
                        static_cast<unsigned int>(dev.function));
            continue;
        }

        if (init_device(dev)) {
            return;
        }
    }

    g_state.initialized = false;
    log_message(LogLevel::Info, "e1000e: no supported device initialized");
}

void poll() {
    if (!g_state.active) {
        return;
    }

    service_device();
    (void)mmio_read32(REG_ICR);
}

bool available() {
    return g_state.active;
}

bool transmit_frame(void* context, const void* data, size_t length) {
    (void)context;
    return transmit(data, length);
}

const uint8_t* mac_address() {
    return g_state.active ? g_state.mac : nullptr;
}

bool transmit(const void* data, size_t length) {
    if (!g_state.active || data == nullptr || length == 0 || length > kPacketBufferSize) {
        return false;
    }

    sync::IrqLockGuard guard(g_tx_lock);

    // Ethernet frames shorter than 64 bytes on the wire must be padded to 60
    // bytes before the NIC appends the FCS.  TCTL.PSP asks the controller to do
    // this, but explicit padding avoids depending on controller-specific short
    // packet behavior.  A bare IPv4/TCP SYN is only 54 bytes.
    constexpr size_t kMinimumFrameWithoutFcs = 60;
    const size_t wire_length =
        length < kMinimumFrameWithoutFcs ? kMinimumFrameWithoutFcs : length;

    reap_tx_locked();

    size_t next_tail = g_state.tx_tail + 1;
    if (next_tail >= kRingCount) {
        next_tail = 0;
    }
    if (next_tail == g_state.tx_head) {
        reap_tx_locked();
        if (next_tail == g_state.tx_head) {
            return false;
        }
    }

    TxDescriptor& desc = g_state.tx_ring[g_state.tx_tail];
    if ((desc.status & TXD_STAT_DD) == 0) {
        return false;
    }

    uint8_t* tx_buffer = g_state.tx_buffer_virt[g_state.tx_tail];
    memcpy(tx_buffer, data, length);
    if (wire_length != length) {
        memset(tx_buffer + length, 0, wire_length - length);
    }
    desc.length = static_cast<uint16_t>(wire_length);
    desc.cso = 0;
    desc.cmd = static_cast<uint8_t>(TXD_CMD_EOP | TXD_CMD_IFCS | TXD_CMD_RS);
    desc.status = 0;
    desc.css = 0;
    desc.special = 0;

    ++g_state.tx_submitted;
    dma_write_barrier();
    g_state.tx_tail = next_tail;
    mmio_write32(REG_TDT0, static_cast<uint32_t>(g_state.tx_tail));
    return true;
}

bool get_debug_info(descriptor_defs::NetDeviceDebug& out) {
    memset(&out, 0, sizeof(out));
    if (!g_state.active) {
        return false;
    }
    out.status = mmio_read32(REG_STATUS);
    out.rctl = mmio_read32(REG_RCTL);
    out.tctl = mmio_read32(REG_TCTL);
    out.rdh = mmio_read32(REG_RDH0);
    out.rdt = mmio_read32(REG_RDT0);
    out.tdh = mmio_read32(REG_TDH0);
    out.tdt = mmio_read32(REG_TDT0);
    out.tx_submitted = g_state.tx_submitted;
    out.tx_completed = g_state.tx_completed;
    out.rx_desc_seen = g_state.rx_desc_seen;
    out.rx_frames_passed = g_state.rx_frames_passed;
    return true;
}

}  // namespace e1000e
