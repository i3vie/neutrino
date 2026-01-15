#include "../descriptor.hpp"

#include "../../drivers/input/mouse.hpp"

namespace descriptor {

namespace descriptor_mouse {

int64_t mouse_read(process::Process&,
                   DescriptorEntry& entry,
                   uint64_t user_address,
                   uint64_t length,
                   uint64_t offset) {
    if (offset != 0) {
        return -1;
    }
    if (length < sizeof(mouse::Event)) {
        return 0;
    }
    auto* buffer = reinterpret_cast<mouse::Event*>(user_address);
    if (buffer == nullptr) {
        return -1;
    }
    uint32_t slot = framebuffer_active_slot();
    size_t max_events = static_cast<size_t>(length / sizeof(mouse::Event));
    size_t count = mouse::read(slot, buffer, max_events);
    return static_cast<int64_t>(count * sizeof(mouse::Event));
}

int64_t mouse_write(process::Process&,
                    DescriptorEntry&,
                    uint64_t,
                    uint64_t,
                    uint64_t) {
    return -1;
}

const Ops kMouseOps{
    .read = mouse_read,
    .write = mouse_write,
    .get_property = nullptr,
    .set_property = nullptr,
};

bool open_mouse(process::Process& proc,
                uint64_t,
                uint64_t,
                uint64_t,
                Allocation& alloc) {
    mouse::init();
    uint32_t slot = 0;
    if (!is_kernel_process(proc)) {
        int32_t proc_slot = framebuffer_slot_for_process(proc);
        if (proc_slot >= 0) {
            slot = static_cast<uint32_t>(proc_slot);
        } else if (console_is_owner(proc)) {
            slot = framebuffer_active_slot();
        } else {
            return false;
        }
    }
    alloc.type = kTypeMouse;
    alloc.flags = static_cast<uint64_t>(Flag::Readable);
    alloc.extended_flags = 0;
    alloc.has_extended_flags = false;
    alloc.object = nullptr;
    alloc.subsystem_data =
        reinterpret_cast<void*>(static_cast<uintptr_t>(slot + 1));
    alloc.close = nullptr;
    alloc.name = "mouse";
    alloc.ops = &kMouseOps;
    return true;
}

}  // namespace descriptor_mouse

bool register_mouse_descriptor() {
    return register_type(kTypeMouse, descriptor_mouse::open_mouse, &descriptor_mouse::kMouseOps);
}

}  // namespace descriptor
