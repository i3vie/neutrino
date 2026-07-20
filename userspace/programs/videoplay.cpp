#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "descriptors.hpp"
#include <neutrino.h>
#include "../crt/syscall.hpp"

namespace {

constexpr uint32_t kWidth = 480;
constexpr uint32_t kHeight = 360;
constexpr uint32_t kFramesPerSecond = 30;
constexpr size_t kVideoFrameBytes = kWidth * kHeight / 4;
constexpr size_t kVideoInputBufferBytes = 4096;
constexpr size_t kAudioBufferBytes = 4096;
constexpr uint64_t kAudioBytesPerSecond = 48000ull * 2 * 2;
constexpr uint64_t kAudioBytesPerVideoFrame =
    kAudioBytesPerSecond / kFramesPerSecond;
constexpr uint32_t kAudioOutput =
    static_cast<uint32_t>(descriptor_defs::Type::AudioOutput);

alignas(16) uint8_t g_video_frame[kVideoFrameBytes];
alignas(16) uint8_t g_audio_buffer[kAudioBufferBytes];

enum class DecodeResult {
    Frame,
    End,
    Invalid,
};

struct RleDecoder {
    uint32_t file;
    uint8_t input[kVideoInputBufferBytes];
    size_t input_position;
    size_t input_size;
    uint8_t run_value;
    uint8_t run_remaining;
    bool read_failed;
};

bool parse_paths(const char* args, char* video, size_t video_capacity,
                 char* audio, size_t audio_capacity) {
    if (args == nullptr) return false;
    char* outputs[2] = {video, audio};
    size_t capacities[2] = {video_capacity, audio_capacity};
    for (size_t item = 0; item < 2; ++item) {
        while (*args == ' ' || *args == '\t') ++args;
        size_t length = 0;
        while (*args != '\0' && *args != ' ' && *args != '\t' &&
               *args != '\r' && *args != '\n') {
            if (length + 1 >= capacities[item]) return false;
            outputs[item][length++] = *args++;
        }
        if (length == 0) return false;
        outputs[item][length] = '\0';
    }
    while (*args == ' ' || *args == '\t' || *args == '\r' || *args == '\n')
        ++args;
    return *args == '\0';
}

bool validate_rle_video(uint32_t file) {
    uint8_t input[kVideoInputBufferBytes];
    bool expecting_value = false;
    uint8_t count = 0;
    uint64_t reconstructed_bytes = 0;
    while (true) {
        long read = file_read(file, input, sizeof(input));
        if (read < 0) return false;
        if (read == 0) break;
        for (long i = 0; i < read; ++i) {
            if (!expecting_value) {
                count = input[i];
                if (count == 0) return false;
                expecting_value = true;
                continue;
            }
            uint64_t next = reconstructed_bytes + count;
            if (next < reconstructed_bytes) return false;
            reconstructed_bytes = next;
            expecting_value = false;
        }
    }
    return !expecting_value && reconstructed_bytes != 0 &&
           reconstructed_bytes % kVideoFrameBytes == 0;
}

bool decoder_read_byte(RleDecoder& decoder, uint8_t& value) {
    if (decoder.input_position == decoder.input_size) {
        long read = file_read(decoder.file, decoder.input,
                              sizeof(decoder.input));
        if (read <= 0) {
            decoder.read_failed = read < 0;
            return false;
        }
        decoder.input_position = 0;
        decoder.input_size = static_cast<size_t>(read);
    }
    value = decoder.input[decoder.input_position++];
    return true;
}

DecodeResult decode_frame(RleDecoder& decoder) {
    size_t output_position = 0;
    while (output_position < kVideoFrameBytes) {
        if (decoder.run_remaining == 0) {
            uint8_t count = 0;
            if (!decoder_read_byte(decoder, count)) {
                if (!decoder.read_failed && output_position == 0)
                    return DecodeResult::End;
                return DecodeResult::Invalid;
            }
            if (count == 0 ||
                !decoder_read_byte(decoder, decoder.run_value))
                return DecodeResult::Invalid;
            decoder.run_remaining = count;
        }
        size_t available = kVideoFrameBytes - output_position;
        size_t chunk = decoder.run_remaining;
        if (chunk > available) chunk = available;
        memset(g_video_frame + output_position, decoder.run_value, chunk);
        output_position += chunk;
        decoder.run_remaining = static_cast<uint8_t>(
            decoder.run_remaining - chunk);
    }
    return DecodeResult::Frame;
}

bool audio_status(uint32_t audio, descriptor_defs::AudioStatusInfo& status) {
    return descriptor_get_property(
               audio,
               static_cast<uint32_t>(descriptor_defs::Property::AudioStatus),
               &status,
               sizeof(status)) == 0;
}

void flush_audio(uint32_t audio) {
    descriptor_defs::AudioControlInfo control{
        descriptor_defs::kAudioCommandFlush, 0};
    (void)descriptor_set_property(
        audio,
        static_cast<uint32_t>(descriptor_defs::Property::AudioControl),
        &control,
        sizeof(control));
}

uint32_t channel_bits(uint8_t gray, uint8_t size, uint8_t shift) {
    if (size == 0 || size > 8 || shift >= 32) return 0;
    uint32_t maximum = (1u << size) - 1u;
    return ((static_cast<uint32_t>(gray) * maximum + 127u) / 255u) << shift;
}

uint32_t gray_pixel(uint8_t gray,
                    const descriptor_defs::FramebufferInfo& info) {
    return channel_bits(gray, info.red_mask_size, info.red_mask_shift) |
           channel_bits(gray, info.green_mask_size, info.green_mask_shift) |
           channel_bits(gray, info.blue_mask_size, info.blue_mask_shift);
}

void store_pixel(uint8_t* output, uint32_t pixel, uint32_t bytes_per_pixel) {
    for (uint32_t byte = 0; byte < bytes_per_pixel; ++byte)
        output[byte] = static_cast<uint8_t>(pixel >> (byte * 8));
}

void draw_frame(uint32_t framebuffer,
                const descriptor_defs::FramebufferInfo& info) {
    uint32_t bytes_per_pixel = (info.bpp + 7u) / 8u;
    uint32_t left = (info.width - kWidth) / 2;
    uint32_t top = (info.height - kHeight) / 2;
    auto* base = reinterpret_cast<uint8_t*>(
        static_cast<uintptr_t>(info.virtual_base));

    for (uint32_t y = 0; y < kHeight; ++y) {
        uint8_t* row = base + static_cast<size_t>(top + y) * info.pitch +
                       static_cast<size_t>(left) * bytes_per_pixel;
        const uint8_t* source = g_video_frame + y * (kWidth / 4);
        for (uint32_t x = 0; x < kWidth; ++x) {
            uint32_t shift = 6u - 2u * (x & 3u);
            uint8_t level = static_cast<uint8_t>((source[x / 4] >> shift) & 3u);
            uint8_t gray = static_cast<uint8_t>(level * 85u);
            store_pixel(row + static_cast<size_t>(x) * bytes_per_pixel,
                        gray_pixel(gray, info), bytes_per_pixel);
        }
    }

    descriptor_defs::FramebufferRect rect{left, top, kWidth, kHeight};
    (void)framebuffer_present(framebuffer, &rect);
}

long open_framebuffer_slot(uint32_t& slot) {
    for (uint32_t candidate = 1; candidate <= 5; ++candidate) {
        long framebuffer = framebuffer_open_slot(candidate);
        if (framebuffer >= 0) {
            slot = candidate;
            return framebuffer;
        }
    }
    return -1;
}

}  // namespace

int main(uint64_t arg_ptr, uint64_t) {
    long console = neutrino_open_stdout();
    char video_path[256]{};
    char audio_path[256]{};
    if (!parse_paths(reinterpret_cast<const char*>(arg_ptr),
                     video_path, sizeof(video_path),
                     audio_path, sizeof(audio_path))) {
        neutrino_write_line(console,
            "usage: videoplay <video.rle> <audio.pcm>");
        neutrino_write_line(console,
            "video: RLE 480x360 2-bit grayscale, 30 fps");
        neutrino_write_line(console,
            "audio: 48000 Hz signed 16-bit stereo LE");
        return 1;
    }

    long video_opened = file_open(video_path);
    if (video_opened < 0) {
        neutrino_write_line(console, "videoplay: unable to open video");
        return 1;
    }
    uint32_t video = static_cast<uint32_t>(video_opened);
    if (!validate_rle_video(video)) {
        neutrino_write_line(console, "videoplay: invalid RLE video");
        file_close(video);
        return 1;
    }
    file_close(video);
    video_opened = file_open(video_path);
    if (video_opened < 0) {
        neutrino_write_line(console, "videoplay: unable to reopen video");
        return 1;
    }
    video = static_cast<uint32_t>(video_opened);
    RleDecoder decoder{video, {}, 0, 0, 0, 0, false};

    long audio_file_opened = file_open(audio_path);
    if (audio_file_opened < 0) {
        neutrino_write_line(console, "videoplay: unable to open audio");
        file_close(video);
        return 1;
    }
    uint32_t audio_file = static_cast<uint32_t>(audio_file_opened);

    long audio_opened = descriptor_open(kAudioOutput, 0);
    if (audio_opened < 0) {
        neutrino_write_line(console, "videoplay: PCM output unavailable");
        file_close(audio_file);
        file_close(video);
        return 1;
    }
    uint32_t audio = static_cast<uint32_t>(audio_opened);

    uint32_t slot = 0;
    long framebuffer_opened = open_framebuffer_slot(slot);
    if (framebuffer_opened < 0) {
        neutrino_write_line(console, "videoplay: no framebuffer slot available");
        descriptor_close(audio);
        file_close(audio_file);
        file_close(video);
        return 1;
    }
    uint32_t framebuffer = static_cast<uint32_t>(framebuffer_opened);
    descriptor_defs::FramebufferInfo info{};
    uint32_t bytes_per_pixel = 0;
    if (framebuffer_get_info(framebuffer, &info) != 0 ||
        info.virtual_base == 0 || info.width < kWidth || info.height < kHeight ||
        info.pitch == 0 ||
        ((bytes_per_pixel = (info.bpp + 7u) / 8u) < 2) ||
        bytes_per_pixel > 4) {
        neutrino_write_line(console, "videoplay: unsupported framebuffer");
        descriptor_close(framebuffer);
        descriptor_close(audio);
        file_close(audio_file);
        file_close(video);
        return 1;
    }

    if (decode_frame(decoder) != DecodeResult::Frame) {
        neutrino_write_line(console, "videoplay: video has no complete frame");
        descriptor_close(framebuffer);
        descriptor_close(audio);
        file_close(audio_file);
        file_close(video);
        return 1;
    }

    memset(reinterpret_cast<void*>(static_cast<uintptr_t>(info.virtual_base)),
           0, static_cast<size_t>(info.pitch) * info.height);
    draw_frame(framebuffer, info);
    neutrino_write_line(console,
        "videoplay: Ctrl+Shift+1 returns to the console");
    if (change_slot(slot) != 0) {
        neutrino_write_line(console, "videoplay: unable to activate framebuffer");
        descriptor_close(framebuffer);
        descriptor_close(audio);
        file_close(audio_file);
        file_close(video);
        return 1;
    }

    bool failed = false;
    bool audio_eof = false;
    bool video_eof = false;
    uint64_t submitted_audio = 0;
    uint64_t displayed_frame = 0;

    while (!video_eof) {
        if (!audio_eof) {
            long read = file_read(audio_file, g_audio_buffer,
                                  sizeof(g_audio_buffer));
            if (read < 0 || (read > 0 && (read & 3) != 0)) {
                failed = true;
                break;
            }
            if (read == 0) {
                audio_eof = true;
            } else {
                long written = descriptor_write(
                    audio, g_audio_buffer, static_cast<size_t>(read));
                if (written != read) {
                    failed = true;
                    break;
                }
                submitted_audio += static_cast<uint64_t>(written);
            }
        }

        descriptor_defs::AudioStatusInfo status{};
        if (!audio_status(audio, status)) {
            failed = true;
            break;
        }
        uint64_t played_audio = submitted_audio > status.queued_bytes
                                    ? submitted_audio - status.queued_bytes
                                    : 0;
        uint64_t wanted_frame = played_audio / kAudioBytesPerVideoFrame;
        bool advanced = false;
        while (displayed_frame < wanted_frame) {
            DecodeResult decoded = decode_frame(decoder);
            if (decoded == DecodeResult::Invalid) {
                failed = true;
                video_eof = true;
                break;
            }
            if (decoded == DecodeResult::End) {
                video_eof = true;
                break;
            }
            ++displayed_frame;
            advanced = true;
        }
        if (!video_eof && advanced)
            draw_frame(framebuffer, info);

        if (audio_eof && status.queued_bytes == 0) break;
        yield();
    }

    if (failed || video_eof) flush_audio(audio);
    descriptor_close(framebuffer);
    descriptor_close(audio);
    file_close(audio_file);
    file_close(video);
    return failed ? 1 : 0;
}
