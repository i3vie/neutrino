#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "descriptors.hpp"
#include <neutrino.h>
#include "../crt/syscall.hpp"

namespace {

constexpr uint32_t kAudioOutput =
    static_cast<uint32_t>(descriptor_defs::Type::AudioOutput);
constexpr size_t kReadSize = 16 * 1024;
constexpr size_t kFrameBytes = 4;

alignas(16) uint8_t g_buffer[kReadSize + kFrameBytes];

bool parse_path(const char* args, char* path, size_t capacity) {
    if (args == nullptr || path == nullptr || capacity == 0) return false;
    while (*args == ' ' || *args == '\t') ++args;
    size_t length = 0;
    while (args[length] != '\0' && args[length] != ' ' &&
           args[length] != '\t' && args[length] != '\r' &&
           args[length] != '\n') {
        if (length + 1 >= capacity) return false;
        path[length] = args[length];
        ++length;
    }
    path[length] = '\0';
    const char* rest = args + length;
    while (*rest == ' ' || *rest == '\t' || *rest == '\r' || *rest == '\n')
        ++rest;
    return length != 0 && *rest == '\0';
}

bool write_all(uint32_t audio, const uint8_t* data, size_t length) {
    size_t written = 0;
    while (written < length) {
        long result = descriptor_write(audio, data + written, length - written);
        if (result <= 0 || (static_cast<size_t>(result) & (kFrameBytes - 1)) != 0)
            return false;
        written += static_cast<size_t>(result);
    }
    return true;
}

}  // namespace

int main(uint64_t arg_ptr, uint64_t) {
    long console = neutrino_open_stdout();
    char path[256]{};
    if (!parse_path(reinterpret_cast<const char*>(arg_ptr), path, sizeof(path))) {
        neutrino_write_line(console, "usage: pcmplay <file.pcm>");
        neutrino_write_line(console,
                            "format: raw 48000 Hz signed 16-bit stereo LE");
        return 1;
    }

    long file = file_open(path);
    if (file < 0) {
        neutrino_write(console, "pcmplay: unable to open ");
        neutrino_write_line(console, path);
        return 1;
    }

    long audio = descriptor_open(kAudioOutput, 0);
    if (audio < 0) {
        neutrino_write_line(console, "pcmplay: HDA PCM output unavailable");
        file_close(static_cast<uint32_t>(file));
        return 1;
    }

    size_t carried = 0;
    bool failed = false;
    for (;;) {
        long result = file_read(static_cast<uint32_t>(file),
                                g_buffer + carried, kReadSize);
        if (result < 0) {
            neutrino_write_line(console, "pcmplay: file read failed");
            failed = true;
            break;
        }
        if (result == 0) {
            if (carried != 0) {
                neutrino_write_line(console,
                                    "pcmplay: truncated PCM frame at end of file");
                failed = true;
            }
            break;
        }

        size_t available = carried + static_cast<size_t>(result);
        size_t playable = available & ~(kFrameBytes - 1);
        if (playable != 0 &&
            !write_all(static_cast<uint32_t>(audio), g_buffer, playable)) {
            neutrino_write_line(console, "pcmplay: audio output failed");
            failed = true;
            break;
        }

        carried = available - playable;
        if (carried != 0) memcpy(g_buffer, g_buffer + playable, carried);
    }

    descriptor_close(static_cast<uint32_t>(audio));
    file_close(static_cast<uint32_t>(file));
    return failed ? 1 : 0;
}
