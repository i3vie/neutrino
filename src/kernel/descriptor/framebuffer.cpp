#include "../descriptor.hpp"

#include "../../drivers/console/console.hpp"

namespace descriptor {

namespace framebuffer_descriptor {

struct FramebufferDescriptor {
    Framebuffer* fb;
    uint64_t physical_base;
    bool locked;
};

FramebufferDescriptor g_framebuffer_descriptor{nullptr, 0, false};
Framebuffer g_framebuffer_storage{};

int64_t framebuffer_read(process::Process&,
                         DescriptorEntry& entry,
                         uint64_t user_address,
                         uint64_t length,
                         uint64_t offset) {
    auto* holder = static_cast<FramebufferDescriptor*>(entry.object);
    if (holder == nullptr || holder->fb == nullptr) {
        return -1;
    }
    Framebuffer* fb = holder->fb;
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
    auto* src = reinterpret_cast<uint8_t*>(fb->base);
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
    auto* holder = static_cast<FramebufferDescriptor*>(entry.object);
    if (holder == nullptr || holder->fb == nullptr) {
        return -1;
    }
    Framebuffer* fb = holder->fb;
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
    auto* dest = reinterpret_cast<uint8_t*>(fb->base);
    auto* src = reinterpret_cast<const uint8_t*>(user_address);
    if (dest == nullptr || src == nullptr) {
        return -1;
    }
    for (uint64_t i = 0; i < length; ++i) {
        dest[offset + i] = src[i];
    }
    return static_cast<int64_t>(length);
}

int framebuffer_get_property(DescriptorEntry& entry,
                             uint32_t property,
                             void* out,
                             size_t size) {
    auto* holder = static_cast<FramebufferDescriptor*>(entry.object);
    if (holder == nullptr || holder->fb == nullptr) {
        return -1;
    }
    Framebuffer* fb = holder->fb;
    if (property ==
        static_cast<uint32_t>(descriptor_defs::Property::FramebufferInfo)) {
        if (out == nullptr || size < sizeof(descriptor_defs::FramebufferInfo)) {
            return -1;
        }
        auto* info = reinterpret_cast<descriptor_defs::FramebufferInfo*>(out);
        info->physical_base = g_framebuffer_descriptor.physical_base;
        info->virtual_base = reinterpret_cast<uint64_t>(fb->base);
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

const Ops kFramebufferOps{
    .read = framebuffer_read,
    .write = framebuffer_write,
    .get_property = framebuffer_get_property,
    .set_property = nullptr,
};

bool open_framebuffer(process::Process& proc,
                      uint64_t index,
                      uint64_t,
                      uint64_t,
                      Allocation& alloc) {
    if (index != 0) {
        return false;
    }
    if (g_framebuffer_descriptor.fb == nullptr) {
        return false;
    }
    bool is_kernel = is_kernel_process(proc);
    if (g_framebuffer_descriptor.locked && !is_kernel) {
        return false;
    }
    alloc.type = kTypeFramebuffer;
    alloc.flags = static_cast<uint64_t>(Flag::Readable) |
                  static_cast<uint64_t>(Flag::Writable) |
                  static_cast<uint64_t>(Flag::Mappable) |
                  static_cast<uint64_t>(Flag::Device);
    alloc.extended_flags = 0;
    alloc.has_extended_flags = false;
    alloc.object = &g_framebuffer_descriptor;
    alloc.subsystem_data = nullptr;
    alloc.close = nullptr;
    alloc.name = "framebuffer";
    alloc.ops = &kFramebufferOps;
    if (is_kernel) {
        g_framebuffer_descriptor.locked = true;
    }
    return true;
}

}  // namespace framebuffer_descriptor

bool register_framebuffer_descriptor() {
    return register_type(kTypeFramebuffer, framebuffer_descriptor::open_framebuffer, &framebuffer_descriptor::kFramebufferOps);
}

void register_framebuffer_device(Framebuffer& framebuffer,
                                 uint64_t physical_base) {
    framebuffer_descriptor::g_framebuffer_storage = framebuffer;
    framebuffer_descriptor::g_framebuffer_descriptor.fb = &framebuffer_descriptor::g_framebuffer_storage;
    framebuffer_descriptor::g_framebuffer_descriptor.physical_base = physical_base;
    framebuffer_descriptor::g_framebuffer_descriptor.locked = false;
}

}  // namespace descriptor
