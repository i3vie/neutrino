#include "../descriptor.hpp"

#include "../../drivers/console/console.hpp"
#include "../../lib/mem.hpp"
#include "../memory/physical_allocator.hpp"
#include "../process.hpp"
#include "../vm.hpp"
#include "arch/x86_64/memory/paging.hpp"

namespace descriptor {

namespace framebuffer_descriptor {

constexpr size_t kFramebufferSlots = 6;
constexpr size_t kPageSize = 0x1000;

struct FramebufferSlot {
    Framebuffer fb;
    uint8_t* buffer;
    size_t buffer_bytes;
    uint64_t physical_base;
    process::Process* owner;
    uint32_t open_count;
    bool kernel_reserved;
};

FramebufferSlot g_framebuffers[kFramebufferSlots]{};
Framebuffer g_hw_fb{};
uint8_t* g_hw_base = nullptr;
size_t g_frame_bytes = 0;
uint32_t g_active_slot = 0;

FramebufferSlot* slot_from_entry(DescriptorEntry& entry) {
    return static_cast<FramebufferSlot*>(entry.object);
}

bool ensure_slot_buffer(FramebufferSlot& slot) {
    if (slot.buffer != nullptr) {
        return true;
    }
    if (g_frame_bytes == 0) {
        return false;
    }
    size_t pages = (g_frame_bytes + kPageSize - 1) / kPageSize;
    uint64_t phys = memory::alloc_kernel_block_pages(pages);
    if (phys == 0) {
        return false;
    }
    auto* start = static_cast<uint8_t*>(paging_phys_to_virt(phys));
    slot.buffer = start;
    slot.buffer_bytes = pages * kPageSize;
    slot.physical_base = phys;
    memset(slot.buffer, 0, slot.buffer_bytes);
    slot.fb = g_hw_fb;
    slot.fb.base = slot.buffer;
    return true;
}

void copy_to_hardware(const FramebufferSlot& slot) {
    if (g_hw_base == nullptr || g_frame_bytes == 0 || slot.buffer == nullptr) {
        return;
    }
    size_t bytes = g_frame_bytes;
    if (slot.buffer_bytes < bytes) {
        bytes = slot.buffer_bytes;
    }
    memcpy_fast(g_hw_base, slot.buffer, bytes);
}

bool copy_rect_to_hardware(const FramebufferSlot& slot,
                           uint32_t x,
                           uint32_t y,
                           uint32_t width,
                           uint32_t height) {
    if (g_hw_base == nullptr || slot.buffer == nullptr) {
        return false;
    }
    const Framebuffer* fb = &slot.fb;
    if (fb->width == 0 || fb->height == 0 || fb->pitch == 0) {
        return false;
    }
    if (x >= fb->width || y >= fb->height) {
        return false;
    }
    if (width == 0 || height == 0) {
        return false;
    }
    if (x + width > fb->width) {
        width = fb->width - x;
    }
    if (y + height > fb->height) {
        height = fb->height - y;
    }
    uint32_t bytes_per_pixel = (fb->bpp + 7u) / 8u;
    if (bytes_per_pixel == 0) {
        return false;
    }
    size_t row_bytes = static_cast<size_t>(width) * bytes_per_pixel;
    for (uint32_t row = 0; row < height; ++row) {
        size_t offset = static_cast<size_t>(y + row) * fb->pitch +
                        static_cast<size_t>(x) * bytes_per_pixel;
        memcpy_fast(g_hw_base + offset, slot.buffer + offset, row_bytes);
    }
    return true;
}

bool map_slot_into_process(process::Process& proc,
                           FramebufferSlot& slot,
                           uint64_t& out_base) {
    if (proc.cr3 == 0 || slot.physical_base == 0 || slot.buffer_bytes == 0) {
        return false;
    }
    vm::Region region = vm::reserve_user_region(slot.buffer_bytes);
    if (region.base == 0 || region.length == 0) {
        return false;
    }
    uint64_t base = region.base;
    uint64_t total = region.length;
    for (uint64_t offset = 0; offset < total; offset += kPageSize) {
        uint64_t phys = slot.physical_base + offset;
        if (!paging_map_page_cr3(proc.cr3,
                                 base + offset,
                                 phys,
                                 PAGE_FLAG_WRITE | PAGE_FLAG_USER)) {
            for (uint64_t rollback = 0; rollback < offset; rollback += kPageSize) {
                uint64_t freed = 0;
                paging_unmap_page_cr3(proc.cr3, base + rollback, freed);
            }
            return false;
        }
    }
    out_base = base;
    return true;
}

int64_t framebuffer_read(process::Process&,
                         DescriptorEntry& entry,
                         uint64_t user_address,
                         uint64_t length,
                         uint64_t offset) {
    auto* slot = slot_from_entry(entry);
    if (slot == nullptr || slot->buffer == nullptr) {
        return -1;
    }
    Framebuffer* fb = &slot->fb;
    size_t frame_bytes = fb->pitch * fb->height;
    if (offset > frame_bytes) {
        return -1;
    }
    if (length == 0) {
        return 0;
    }
    if (offset + length > frame_bytes) {
        return -1;
    }
    auto* src = slot->buffer;
    auto* dest = reinterpret_cast<uint8_t*>(user_address);
    if (src == nullptr || dest == nullptr) {
        return -1;
    }
    for (uint64_t i = 0; i < length; ++i) {
        dest[i] = src[offset + i];
    }
    return static_cast<int64_t>(length);
}

int64_t framebuffer_write(process::Process&,
                          DescriptorEntry& entry,
                          uint64_t user_address,
                          uint64_t length,
                          uint64_t offset) {
    auto* slot = slot_from_entry(entry);
    if (slot == nullptr || slot->buffer == nullptr) {
        return -1;
    }
    Framebuffer* fb = &slot->fb;
    size_t frame_bytes = fb->pitch * fb->height;
    if (offset > frame_bytes) {
        return -1;
    }
    if (length == 0) {
        return 0;
    }
    if (offset + length > frame_bytes) {
        return -1;
    }
    auto* dest = slot->buffer;
    auto* src = reinterpret_cast<const uint8_t*>(user_address);
    if (dest == nullptr || src == nullptr) {
        return -1;
    }
    for (uint64_t i = 0; i < length; ++i) {
        dest[offset + i] = src[i];
    }
    uint32_t slot_index =
        static_cast<uint32_t>(slot - g_framebuffers);
    if (g_active_slot == slot_index && g_hw_base != nullptr &&
        slot->buffer != g_hw_base) {
        memcpy_fast(g_hw_base + offset, dest + offset, length);
    }
    return static_cast<int64_t>(length);
}

int framebuffer_get_property(DescriptorEntry& entry,
                             uint32_t property,
                             void* out,
                             size_t size) {
    auto* slot = slot_from_entry(entry);
    if (slot == nullptr || slot->buffer == nullptr) {
        return -1;
    }
    Framebuffer* fb = &slot->fb;
    if (property ==
        static_cast<uint32_t>(descriptor_defs::Property::FramebufferInfo)) {
        if (out == nullptr || size < sizeof(descriptor_defs::FramebufferInfo)) {
            return -1;
        }
        auto* info = reinterpret_cast<descriptor_defs::FramebufferInfo*>(out);
        info->physical_base = slot->physical_base;
        if (entry.subsystem_data != nullptr) {
            info->virtual_base =
                reinterpret_cast<uint64_t>(entry.subsystem_data);
        } else {
            info->virtual_base =
                reinterpret_cast<uint64_t>(slot->buffer);
        }
        info->width = static_cast<uint32_t>(fb->width);
        info->height = static_cast<uint32_t>(fb->height);
        info->pitch = static_cast<uint32_t>(fb->pitch);
        info->bpp = fb->bpp;
        info->memory_model = fb->memory_model;
        info->reserved = 0;
        info->red_mask_size = fb->red_mask_size;
        info->red_mask_shift = fb->red_mask_shift;
        info->green_mask_size = fb->green_mask_size;
        info->green_mask_shift = fb->green_mask_shift;
        info->blue_mask_size = fb->blue_mask_size;
        info->blue_mask_shift = fb->blue_mask_shift;
        return 0;
    }
    return -1;
}

int framebuffer_set_property(DescriptorEntry& entry,
                             uint32_t property,
                             const void* in,
                             size_t size) {
    if (property !=
        static_cast<uint32_t>(descriptor_defs::Property::FramebufferPresent)) {
        return -1;
    }
    auto* slot = slot_from_entry(entry);
    if (slot == nullptr || slot->buffer == nullptr) {
        return -1;
    }
    uint32_t slot_index =
        static_cast<uint32_t>(slot - g_framebuffers);
    if (g_active_slot != slot_index) {
        return -1;
    }
    if (size == 0 || in == nullptr) {
        copy_to_hardware(*slot);
        return 0;
    }
    if (size < sizeof(descriptor_defs::FramebufferRect)) {
        return -1;
    }
    auto* rect =
        reinterpret_cast<const descriptor_defs::FramebufferRect*>(in);
    return copy_rect_to_hardware(*slot,
                                 rect->x,
                                 rect->y,
                                 rect->width,
                                 rect->height)
               ? 0
               : -1;
}

void framebuffer_close(DescriptorEntry& entry) {
    auto* slot = slot_from_entry(entry);
    if (slot == nullptr) {
        return;
    }
    if (slot->open_count > 0) {
        --slot->open_count;
    }
    if (slot->open_count == 0 && !slot->kernel_reserved) {
        slot->owner = nullptr;
    }
}

const Ops kFramebufferOps{
    .read = framebuffer_read,
    .write = framebuffer_write,
    .get_property = framebuffer_get_property,
    .set_property = framebuffer_set_property,
};

FramebufferSlot* allocate_user_slot(process::Process& proc) {
    for (size_t i = 1; i < kFramebufferSlots; ++i) {
        FramebufferSlot& slot = g_framebuffers[i];
        if (slot.owner == nullptr || slot.owner == &proc) {
            return &slot;
        }
    }
    return nullptr;
}

bool open_framebuffer(process::Process& proc,
                      uint64_t arg0,
                      uint64_t,
                      uint64_t,
                      Allocation& alloc) {
    bool is_kernel = is_kernel_process(proc);
    FramebufferSlot* slot = nullptr;
    if (arg0 != 0) {
        uint32_t index = static_cast<uint32_t>(arg0);
        if (index >= kFramebufferSlots) {
            return false;
        }
        slot = &g_framebuffers[index];
    } else if (is_kernel) {
        slot = &g_framebuffers[0];
    } else {
        slot = allocate_user_slot(proc);
    }
    if (slot == nullptr) {
        return false;
    }
    if (!is_kernel && slot->kernel_reserved) {
        return false;
    }
    if (slot->owner != nullptr && slot->owner != &proc) {
        return false;
    }
    if (!ensure_slot_buffer(*slot)) {
        return false;
    }
    uint64_t mapped_base = 0;
    if (!is_kernel) {
        if (!map_slot_into_process(proc, *slot, mapped_base)) {
            return false;
        }
    }
    slot->owner = &proc;
    ++slot->open_count;
    alloc.type = kTypeFramebuffer;
    alloc.flags = static_cast<uint64_t>(Flag::Readable) |
                  static_cast<uint64_t>(Flag::Writable) |
                  static_cast<uint64_t>(Flag::Mappable) |
                  static_cast<uint64_t>(Flag::Device);
    alloc.extended_flags = 0;
    alloc.has_extended_flags = false;
    alloc.object = slot;
    if (is_kernel) {
        alloc.subsystem_data = slot->buffer;
    } else {
        alloc.subsystem_data =
            reinterpret_cast<void*>(mapped_base);
    }
    alloc.close = framebuffer_close;
    alloc.name = "framebuffer";
    alloc.ops = &kFramebufferOps;
    return true;
}

}  // namespace framebuffer_descriptor

bool register_framebuffer_descriptor() {
    return register_type(kTypeFramebuffer, framebuffer_descriptor::open_framebuffer, &framebuffer_descriptor::kFramebufferOps);
}

void register_framebuffer_device(Framebuffer& framebuffer,
                                 uint64_t physical_base) {
    using namespace framebuffer_descriptor;
    g_hw_fb = framebuffer;
    g_hw_base = framebuffer.base;
    g_frame_bytes = (framebuffer.pitch != 0)
                        ? framebuffer.pitch * framebuffer.height
                        : 0;
    for (size_t i = 0; i < kFramebufferSlots; ++i) {
        FramebufferSlot& slot = g_framebuffers[i];
        slot.fb = framebuffer;
        slot.buffer = nullptr;
        slot.buffer_bytes = 0;
        slot.owner = nullptr;
        slot.open_count = 0;
        slot.kernel_reserved = (i == 0);
        slot.physical_base = (i == 0) ? physical_base : 0;
    }
    g_framebuffers[0].buffer = g_hw_base;
    g_framebuffers[0].buffer_bytes = g_frame_bytes;
    g_active_slot = 0;
}

void framebuffer_select(uint32_t index) {
    using namespace framebuffer_descriptor;
    if (index >= kFramebufferSlots) {
        return;
    }
    if (g_frame_bytes == 0 || g_hw_base == nullptr) {
        return;
    }
    if (index != 0) {
        if (!ensure_slot_buffer(g_framebuffers[index])) {
            return;
        }
    }
    g_active_slot = index;
    if (index == 0) {
        if (kconsole != nullptr) {
            kconsole->present();
        } else {
            copy_to_hardware(g_framebuffers[0]);
        }
    } else {
        copy_to_hardware(g_framebuffers[index]);
    }
}

bool framebuffer_is_active(uint32_t index) {
    using namespace framebuffer_descriptor;
    return g_active_slot == index;
}

uint32_t framebuffer_active_slot() {
    using namespace framebuffer_descriptor;
    return g_active_slot;
}

int32_t framebuffer_slot_for_process(const process::Process& proc) {
    using namespace framebuffer_descriptor;
    if (is_kernel_process(proc)) {
        return 0;
    }
    for (size_t i = 1; i < kFramebufferSlots; ++i) {
        if (g_framebuffers[i].owner == &proc) {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

}  // namespace descriptor
