#include "kernel/descriptor.hpp"

#include "drivers/audio/hda.hpp"

namespace descriptor {
namespace audio_output_descriptor {

int64_t read(process::Process&, DescriptorEntry&, uint64_t, uint64_t, uint64_t) {
    return -1;
}

int64_t write(process::Process&, DescriptorEntry&, uint64_t address,
              uint64_t length, uint64_t offset) {
    if (offset != 0 || (length & 3u) != 0) return -1;
    if (length == 0) return 0;
    const void* samples = reinterpret_cast<const void*>(address);
    if (samples == nullptr) return -1;
    return static_cast<int64_t>(hda::write_pcm(samples, static_cast<size_t>(length)));
}

int get_property(DescriptorEntry&, uint32_t property, void* out, size_t size) {
    if (property != static_cast<uint32_t>(descriptor_defs::Property::AudioFormat) ||
        out == nullptr || size < sizeof(descriptor_defs::AudioFormatInfo))
        return -1;
    auto* format = static_cast<descriptor_defs::AudioFormatInfo*>(out);
    *format = descriptor_defs::AudioFormatInfo{48000, 2, 16, 4, 0};
    return 0;
}

const Ops kOps{
    .read = read,
    .write = write,
    .get_property = get_property,
    .set_property = nullptr,
};

bool open(process::Process&, uint64_t selector, uint64_t, uint64_t,
          Allocation& allocation) {
    if (selector != 0 || !hda::available()) return false;
    allocation.type = kTypeAudioOutput;
    allocation.flags = static_cast<uint64_t>(Flag::Writable) |
                       static_cast<uint64_t>(Flag::Device) |
                       static_cast<uint64_t>(Flag::CapStream);
    allocation.extended_flags = 0;
    allocation.has_extended_flags = false;
    allocation.object = nullptr;
    allocation.subsystem_data = nullptr;
    allocation.name = "hda-pcm-out";
    allocation.ops = &kOps;
    allocation.ext = nullptr;
    allocation.close = nullptr;
    return true;
}

}  // namespace audio_output_descriptor

bool register_audio_output_descriptor() {
    return register_type(kTypeAudioOutput, audio_output_descriptor::open,
                         &audio_output_descriptor::kOps);
}

}  // namespace descriptor
