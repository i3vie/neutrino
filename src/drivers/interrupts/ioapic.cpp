#include "ioapic.hpp"

#include "../log/logging.hpp"
#include "arch/x86_64/memory/paging.hpp"

namespace {

struct [[gnu::packed]] RsdpV1 {
    char signature[8];
    uint8_t checksum;
    char oem_id[6];
    uint8_t revision;
    uint32_t rsdt_address;
};

struct [[gnu::packed]] RsdpV2 {
    RsdpV1 first_part;
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t extended_checksum;
    uint8_t reserved[3];
};

struct [[gnu::packed]] AcpiSdtHeader {
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oem_id[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
};

struct [[gnu::packed]] Madt {
    AcpiSdtHeader header;
    uint32_t lapic_address;
    uint32_t flags;
};

struct [[gnu::packed]] MadtEntryHeader {
    uint8_t type;
    uint8_t length;
};

struct [[gnu::packed]] MadtIoApic {
    MadtEntryHeader header;
    uint8_t ioapic_id;
    uint8_t reserved;
    uint32_t ioapic_address;
    uint32_t gsi_base;
};

struct [[gnu::packed]] MadtIso {
    MadtEntryHeader header;
    uint8_t bus_source;
    uint8_t irq_source;
    uint32_t gsi;
    uint16_t flags;
};

constexpr size_t kMaxIoApics = 4;
constexpr size_t kMaxOverrides = 16;
constexpr uint32_t kIoApicRegisterSelect = 0x00;
constexpr uint32_t kIoApicWindow = 0x10;
constexpr uint32_t kIoApicRegVersion = 0x01;
constexpr uint32_t kIoApicRedirBase = 0x10;
constexpr uint64_t kPolarityMask = 0x00002000ull;
constexpr uint64_t kTriggerMask = 0x00008000ull;
constexpr uint64_t kMaskBit = 1ull << 16;
constexpr uint64_t kDestShift = 56;

struct IoApicInfo {
    bool present;
    uint32_t gsi_base;
    uint32_t redir_count;
    volatile uint32_t* regs;
};

struct IsoOverride {
    bool present;
    uint8_t source_irq;
    uint32_t gsi;
    uint16_t flags;
};

IoApicInfo g_ioapics[kMaxIoApics]{};
IsoOverride g_overrides[kMaxOverrides]{};
bool g_available = false;
bool g_handles_irq[16]{};
uint32_t g_lapic_id = 0;

bool match_signature(const char* lhs, const char* rhs, size_t length) {
    if (lhs == nullptr || rhs == nullptr) {
        return false;
    }
    for (size_t i = 0; i < length; ++i) {
        if (lhs[i] != rhs[i]) {
            return false;
        }
    }
    return true;
}

bool checksum_ok(const void* data, size_t length) {
    if (data == nullptr || length == 0) {
        return false;
    }
    const auto* bytes = static_cast<const uint8_t*>(data);
    uint8_t sum = 0;
    for (size_t i = 0; i < length; ++i) {
        sum = static_cast<uint8_t>(sum + bytes[i]);
    }
    return sum == 0;
}

volatile uint32_t* map_mmio(uint32_t phys_addr, uint64_t hhdm_offset) {
    uint64_t virt_addr = static_cast<uint64_t>(phys_addr) + hhdm_offset;
    uint64_t flags =
        PAGE_FLAG_WRITE | PAGE_FLAG_CACHE_DISABLE | PAGE_FLAG_WRITE_THROUGH;
    (void)paging_map_page(static_cast<uint64_t>(phys_addr),
                          static_cast<uint64_t>(phys_addr),
                          flags);
    (void)paging_map_page(virt_addr, static_cast<uint64_t>(phys_addr), flags);
    return reinterpret_cast<volatile uint32_t*>(virt_addr);
}

uint32_t ioapic_read(volatile uint32_t* regs, uint32_t reg) {
    regs[kIoApicRegisterSelect / sizeof(uint32_t)] = reg;
    return regs[kIoApicWindow / sizeof(uint32_t)];
}

void ioapic_write(volatile uint32_t* regs, uint32_t reg, uint32_t value) {
    regs[kIoApicRegisterSelect / sizeof(uint32_t)] = reg;
    regs[kIoApicWindow / sizeof(uint32_t)] = value;
}

bool register_ioapic(const MadtIoApic& entry, uint64_t hhdm_offset) {
    for (size_t i = 0; i < kMaxIoApics; ++i) {
        if (g_ioapics[i].present) {
            continue;
        }
        volatile uint32_t* regs = map_mmio(entry.ioapic_address, hhdm_offset);
        uint32_t version = ioapic_read(regs, kIoApicRegVersion);
        uint32_t max_redir = (version >> 16) & 0xFFu;
        g_ioapics[i].present = true;
        g_ioapics[i].gsi_base = entry.gsi_base;
        g_ioapics[i].redir_count = max_redir + 1;
        g_ioapics[i].regs = regs;
        return true;
    }
    return false;
}

void register_override(const MadtIso& entry) {
    if (entry.bus_source != 0) {
        return;
    }
    for (size_t i = 0; i < kMaxOverrides; ++i) {
        if (g_overrides[i].present) {
            continue;
        }
        g_overrides[i].present = true;
        g_overrides[i].source_irq = entry.irq_source;
        g_overrides[i].gsi = entry.gsi;
        g_overrides[i].flags = entry.flags;
        return;
    }
}

const IsoOverride* find_override(uint8_t irq) {
    for (size_t i = 0; i < kMaxOverrides; ++i) {
        if (g_overrides[i].present && g_overrides[i].source_irq == irq) {
            return &g_overrides[i];
        }
    }
    return nullptr;
}

IoApicInfo* find_ioapic_for_gsi(uint32_t gsi) {
    for (size_t i = 0; i < kMaxIoApics; ++i) {
        IoApicInfo& ioapic = g_ioapics[i];
        if (!ioapic.present) {
            continue;
        }
        if (gsi >= ioapic.gsi_base &&
            gsi < ioapic.gsi_base + ioapic.redir_count) {
            return &ioapic;
        }
    }
    return nullptr;
}

const AcpiSdtHeader* find_madt_from_sdt(const AcpiSdtHeader* sdt, bool xsdt) {
    if (sdt == nullptr || sdt->length < sizeof(AcpiSdtHeader)) {
        return nullptr;
    }
    if (!checksum_ok(sdt, sdt->length)) {
        return nullptr;
    }
    size_t entry_size = xsdt ? sizeof(uint64_t) : sizeof(uint32_t);
    size_t payload_length = sdt->length - sizeof(AcpiSdtHeader);
    size_t entry_count = payload_length / entry_size;
    const uint8_t* entry_base =
        reinterpret_cast<const uint8_t*>(sdt) + sizeof(AcpiSdtHeader);
    for (size_t i = 0; i < entry_count; ++i) {
        const AcpiSdtHeader* child = nullptr;
        if (xsdt) {
            auto address =
                reinterpret_cast<const uint64_t*>(entry_base)[i];
            child = reinterpret_cast<const AcpiSdtHeader*>(
                static_cast<uintptr_t>(address));
        } else {
            auto address =
                reinterpret_cast<const uint32_t*>(entry_base)[i];
            child = reinterpret_cast<const AcpiSdtHeader*>(
                static_cast<uintptr_t>(address));
        }
        if (child != nullptr && match_signature(child->signature, "APIC", 4)) {
            return child;
        }
    }
    return nullptr;
}

const Madt* find_madt(uint64_t rsdp_address) {
    if (rsdp_address == 0) {
        return nullptr;
    }
    const auto* rsdp = reinterpret_cast<const RsdpV1*>(
        static_cast<uintptr_t>(rsdp_address));
    if (!match_signature(rsdp->signature, "RSD PTR ", 8) ||
        !checksum_ok(rsdp, sizeof(RsdpV1))) {
        return nullptr;
    }

    if (rsdp->revision >= 2) {
        const auto* rsdp2 = reinterpret_cast<const RsdpV2*>(rsdp);
        if (rsdp2->length < sizeof(RsdpV2) ||
            !checksum_ok(rsdp2, rsdp2->length)) {
            return nullptr;
        }
        if (rsdp2->xsdt_address != 0) {
            const auto* xsdt = reinterpret_cast<const AcpiSdtHeader*>(
                static_cast<uintptr_t>(rsdp2->xsdt_address));
            if (const auto* madt = find_madt_from_sdt(xsdt, true)) {
                return reinterpret_cast<const Madt*>(madt);
            }
        }
    }

    if (rsdp->rsdt_address != 0) {
        const auto* rsdt = reinterpret_cast<const AcpiSdtHeader*>(
            static_cast<uintptr_t>(rsdp->rsdt_address));
        if (const auto* madt = find_madt_from_sdt(rsdt, false)) {
            return reinterpret_cast<const Madt*>(madt);
        }
    }
    return nullptr;
}

}  // namespace

namespace ioapic {

void init(uint64_t rsdp_address, uint64_t hhdm_offset, uint32_t lapic_id) {
    g_available = false;
    g_lapic_id = lapic_id;
    for (size_t i = 0; i < kMaxIoApics; ++i) {
        g_ioapics[i] = {};
    }
    for (size_t i = 0; i < kMaxOverrides; ++i) {
        g_overrides[i] = {};
    }
    for (size_t i = 0; i < 16; ++i) {
        g_handles_irq[i] = false;
    }

    const Madt* madt = find_madt(rsdp_address);
    if (madt == nullptr || madt->header.length < sizeof(Madt)) {
        log_message(LogLevel::Info, "IOAPIC: MADT not available");
        return;
    }

    const uint8_t* cursor =
        reinterpret_cast<const uint8_t*>(madt) + sizeof(Madt);
    const uint8_t* end =
        reinterpret_cast<const uint8_t*>(madt) + madt->header.length;
    while (cursor + sizeof(MadtEntryHeader) <= end) {
        const auto* header = reinterpret_cast<const MadtEntryHeader*>(cursor);
        if (header->length < sizeof(MadtEntryHeader) ||
            cursor + header->length > end) {
            break;
        }

        if (header->type == 1 &&
            header->length >= sizeof(MadtIoApic)) {
            register_ioapic(*reinterpret_cast<const MadtIoApic*>(cursor),
                            hhdm_offset);
        } else if (header->type == 2 &&
                   header->length >= sizeof(MadtIso)) {
            register_override(*reinterpret_cast<const MadtIso*>(cursor));
        }
        cursor += header->length;
    }

    for (size_t i = 0; i < kMaxIoApics; ++i) {
        if (g_ioapics[i].present) {
            g_available = true;
            break;
        }
    }

    if (g_available) {
        log_message(LogLevel::Info, "IOAPIC: discovered controller(s)");
    } else {
        log_message(LogLevel::Info, "IOAPIC: no controllers discovered");
    }
}

bool available() {
    return g_available;
}

bool route_isa_irq(uint8_t irq, uint8_t vector) {
    if (!g_available || irq >= 16) {
        return false;
    }

    uint32_t gsi = irq;
    uint16_t flags = 0;
    if (const auto* override = find_override(irq)) {
        gsi = override->gsi;
        flags = override->flags;
    }

    IoApicInfo* ioapic = find_ioapic_for_gsi(gsi);
    if (ioapic == nullptr || ioapic->regs == nullptr) {
        return false;
    }

    uint32_t redir_index = gsi - ioapic->gsi_base;
    uint32_t reg = kIoApicRedirBase + redir_index * 2;
    uint64_t entry = vector;

    uint16_t polarity = flags & 0x3;
    uint16_t trigger = (flags >> 2) & 0x3;
    if (polarity == 3) {
        entry |= kPolarityMask;
    }
    if (trigger == 3) {
        entry |= kTriggerMask;
    }
    entry |= static_cast<uint64_t>(g_lapic_id) << kDestShift;

    ioapic_write(ioapic->regs, reg + 1, static_cast<uint32_t>(entry >> 32));
    ioapic_write(ioapic->regs, reg, static_cast<uint32_t>(entry & 0xFFFFFFFFu));
    g_handles_irq[irq] = true;

    log_message(LogLevel::Info,
                "IOAPIC: routed ISA IRQ %u via GSI %u to vector %u",
                static_cast<unsigned int>(irq),
                static_cast<unsigned int>(gsi),
                static_cast<unsigned int>(vector));
    return true;
}

bool handles_irq(uint8_t irq) {
    return irq < 16 && g_handles_irq[irq];
}

}  // namespace ioapic
