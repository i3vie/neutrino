#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "descriptors.hpp"
#include "keyboard_scancode.hpp"
#include <neutrino.h>
#include "../crt/syscall.hpp"

namespace {

constexpr uint32_t kAudioOutput =
    static_cast<uint32_t>(descriptor_defs::Type::AudioOutput);
constexpr uint32_t kKeyboard =
    static_cast<uint32_t>(descriptor_defs::Type::Keyboard);
constexpr size_t kBufferBytes = 4096;
constexpr size_t kFrameBytes = 4;
constexpr uint64_t kBytesPerSecond = 48000ull * 2 * 2;
constexpr uint64_t kSeekBytes = 5 * kBytesPerSecond;

alignas(16) uint8_t g_buffer[kBufferBytes];
alignas(16) uint8_t g_discard[kBufferBytes];

enum class Action {
    None,
    TogglePause,
    Quit,
    SeekBack,
    SeekForward,
    VolumeUp,
    VolumeDown,
};

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

bool audio_control(uint32_t audio, uint32_t command, int32_t value = 0) {
    descriptor_defs::AudioControlInfo control{command, value};
    return descriptor_set_property(
               audio,
               static_cast<uint32_t>(descriptor_defs::Property::AudioControl),
               &control,
               sizeof(control)) == 0;
}

bool audio_status(uint32_t audio, descriptor_defs::AudioStatusInfo& status) {
    return descriptor_get_property(
               audio,
               static_cast<uint32_t>(descriptor_defs::Property::AudioStatus),
               &status,
               sizeof(status)) == 0;
}

void print_volume(long console, uint32_t volume) {
    char text[] = "volume: 000%";
    text[8] = static_cast<char>('0' + (volume / 100) % 10);
    text[9] = static_cast<char>('0' + (volume / 10) % 10);
    text[10] = static_cast<char>('0' + volume % 10);
    neutrino_write_line(console, text);
}

Action action_for_char(uint8_t ch) {
    switch (ch) {
        case ' ':
        case 'p':
        case 'P': return Action::TogglePause;
        case 'q':
        case 'Q': return Action::Quit;
        case '+':
        case '=': return Action::VolumeUp;
        case '-':
        case '_': return Action::VolumeDown;
        default: return Action::None;
    }
}

Action poll_input(uint32_t input, bool keyboard_input, uint8_t& escape_state) {
    if (keyboard_input) {
        descriptor_defs::KeyboardEvent events[8]{};
        long count_bytes = descriptor_read(input, events, sizeof(events));
        if (count_bytes <= 0) return Action::None;
        size_t count = static_cast<size_t>(count_bytes) / sizeof(events[0]);
        for (size_t i = 0; i < count; ++i) {
            if (!keyboard::is_pressed(events[i])) continue;
            if (keyboard::is_extended(events[i])) {
                switch (events[i].scancode) {
                    case keyboard::kScancodeLeft: return Action::SeekBack;
                    case keyboard::kScancodeRight: return Action::SeekForward;
                    case keyboard::kScancodeUp: return Action::VolumeUp;
                    case keyboard::kScancodeDown: return Action::VolumeDown;
                    default: continue;
                }
            }
            char ch = keyboard::scancode_to_char(events[i].scancode,
                                                  events[i].mods);
            Action action = action_for_char(static_cast<uint8_t>(ch));
            if (action != Action::None) return action;
        }
        return Action::None;
    }

    uint8_t bytes[16];
    long count = descriptor_read(input, bytes, sizeof(bytes));
    if (count <= 0) return Action::None;
    for (long i = 0; i < count; ++i) {
        uint8_t ch = bytes[i];
        if (escape_state == 1) {
            escape_state = ch == '[' ? 2 : 0;
            continue;
        }
        if (escape_state == 2) {
            escape_state = 0;
            if (ch == 'A') return Action::VolumeUp;
            if (ch == 'B') return Action::VolumeDown;
            if (ch == 'C') return Action::SeekForward;
            if (ch == 'D') return Action::SeekBack;
            continue;
        }
        if (ch == 0x1B) {
            escape_state = 1;
            continue;
        }
        Action action = action_for_char(ch);
        if (action != Action::None) return action;
    }
    return Action::None;
}

bool seek_file(const char* path, uint32_t& file, uint64_t target,
               uint64_t& actual) {
    long replacement = file_open(path);
    if (replacement < 0) return false;
    actual = 0;
    target &= ~(static_cast<uint64_t>(kFrameBytes) - 1);
    while (actual < target) {
        size_t wanted = static_cast<size_t>(target - actual);
        if (wanted > sizeof(g_discard)) wanted = sizeof(g_discard);
        long result = file_read(static_cast<uint32_t>(replacement),
                                g_discard, wanted);
        if (result < 0) {
            file_close(static_cast<uint32_t>(replacement));
            return false;
        }
        if (result == 0) break;
        actual += static_cast<uint64_t>(result);
    }
    actual &= ~(static_cast<uint64_t>(kFrameBytes) - 1);
    file_close(file);
    file = static_cast<uint32_t>(replacement);
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

    long opened_file = file_open(path);
    if (opened_file < 0) {
        neutrino_write(console, "pcmplay: unable to open ");
        neutrino_write_line(console, path);
        return 1;
    }
    uint32_t file = static_cast<uint32_t>(opened_file);

    long opened_audio = descriptor_open(kAudioOutput, 0);
    if (opened_audio < 0) {
        neutrino_write_line(console, "pcmplay: HDA PCM output unavailable");
        file_close(file);
        return 1;
    }
    uint32_t audio = static_cast<uint32_t>(opened_audio);

    long input_handle = process_get_standard_descriptor(0);
    bool keyboard_input = false;
    if (input_handle < 0) {
        input_handle = descriptor_open(kKeyboard, 0);
        keyboard_input = input_handle >= 0;
    }

    neutrino_write_line(console,
        "Space play/pause | Left/Right seek 5s | Up/Down volume | Q quit");

    descriptor_defs::AudioStatusInfo status{};
    uint32_t volume = audio_status(audio, status) ? status.volume : 100;
    bool paused = false;
    bool eof = false;
    bool failed = false;
    bool quit = false;
    uint64_t submitted = 0;
    uint8_t escape_state = 0;

    while (!quit) {
        Action action = input_handle >= 0
                            ? poll_input(static_cast<uint32_t>(input_handle),
                                         keyboard_input, escape_state)
                            : Action::None;
        if (action == Action::Quit) {
            (void)audio_control(audio, descriptor_defs::kAudioCommandFlush);
            quit = true;
            break;
        }
        if (action == Action::TogglePause) {
            paused = !paused;
            (void)audio_control(audio,
                                paused ? descriptor_defs::kAudioCommandPause
                                       : descriptor_defs::kAudioCommandResume);
            neutrino_write_line(console, paused ? "paused" : "playing");
        } else if (action == Action::VolumeUp ||
                   action == Action::VolumeDown) {
            int next = static_cast<int>(volume) +
                       (action == Action::VolumeUp ? 5 : -5);
            if (next < 0) next = 0;
            if (next > 100) next = 100;
            if (audio_control(audio, descriptor_defs::kAudioCommandSetVolume,
                              next)) {
                volume = static_cast<uint32_t>(next);
                print_volume(console, volume);
            }
        } else if (action == Action::SeekBack ||
                   action == Action::SeekForward) {
            if (audio_status(audio, status)) {
                uint64_t heard = submitted > status.queued_bytes
                                     ? submitted - status.queued_bytes
                                     : 0;
                uint64_t target = action == Action::SeekBack
                                      ? (heard > kSeekBytes
                                             ? heard - kSeekBytes
                                             : 0)
                                      : heard + kSeekBytes;
                (void)audio_control(audio, descriptor_defs::kAudioCommandFlush);
                uint64_t actual = 0;
                if (seek_file(path, file, target, actual)) {
                    submitted = actual;
                    eof = false;
                    if (paused)
                        (void)audio_control(
                            audio, descriptor_defs::kAudioCommandPause);
                    neutrino_write_line(console, "seeked");
                } else {
                    failed = true;
                    break;
                }
            }
        }

        if (!paused && !eof) {
            long result = file_read(file, g_buffer, sizeof(g_buffer));
            if (result < 0) {
                neutrino_write_line(console, "pcmplay: file read failed");
                failed = true;
                break;
            }
            if (result == 0) {
                eof = true;
            } else {
                size_t playable = static_cast<size_t>(result) &
                                  ~(kFrameBytes - 1);
                if (playable != static_cast<size_t>(result)) {
                    neutrino_write_line(console,
                        "pcmplay: truncated PCM frame at end of file");
                    failed = true;
                    break;
                }
                long written = descriptor_write(audio, g_buffer, playable);
                if (written != static_cast<long>(playable)) {
                    neutrino_write_line(console, "pcmplay: audio output failed");
                    failed = true;
                    break;
                }
                submitted += playable;
            }
        }

        if (eof && !paused && audio_status(audio, status) &&
            status.queued_bytes == 0)
            break;
        yield();
    }

    if (failed) (void)audio_control(audio, descriptor_defs::kAudioCommandFlush);
    descriptor_close(audio);
    file_close(file);
    if (keyboard_input && input_handle >= 0)
        descriptor_close(static_cast<uint32_t>(input_handle));
    return failed ? 1 : 0;
}
