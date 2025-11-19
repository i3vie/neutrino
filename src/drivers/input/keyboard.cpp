#include "keyboard.hpp"

#include "arch/x86_64/io.hpp"
#include "../interrupts/pic.hpp"
#include "../log/logging.hpp"

namespace keyboard {
namespace {

constexpr size_t kBufferSize = 256;
char g_buffer[kBufferSize];
size_t g_head = 0;
size_t g_tail = 0;

bool g_shift = false;
bool g_caps_lock = false;
bool g_initialized = false;
bool g_left_shift_down = false;
bool g_right_shift_down = false;

constexpr char kScancodeMap[129] = {
    0,
    27,  '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b', '\t',
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0,  'a',
    's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0,  '\\', 'z', 'x',
    'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,   '*', 0,   ' ', 0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   '7', '8', '9', '-', '4', '5', '6', '+',
    '1', '2', '3', '0', '.', 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0
};

constexpr char kScancodeShiftMap[129] = {
    0,
    27,  '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b', '\t',
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0,  'A',
    'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0,  '|', 'Z', 'X',
    'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,   '*', 0,   ' ', 0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   '7', '8', '9', '-', '4', '5', '6', '+',
    '1', '2', '3', '0', '.', 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0
};

void enqueue(char ch) {
    size_t next = (g_head + 1) % kBufferSize;
    if (next == g_tail) {
        return;
    }
    g_buffer[g_head] = ch;
    g_head = next;
}

bool dequeue(char& ch) {
    if (g_head == g_tail) {
        return false;
    }
    ch = g_buffer[g_tail];
    g_tail = (g_tail + 1) % kBufferSize;
    return true;
}

void update_shift_state() {
    g_shift = g_left_shift_down || g_right_shift_down;
}

}  // namespace

void init() {
    if (g_initialized) {
        return;
    }
    g_head = 0;
    g_tail = 0;
    g_shift = false;
    g_caps_lock = false;
    g_left_shift_down = false;
    g_right_shift_down = false;
    g_initialized = true;

    pic::set_mask(1, false);
}

void handle_irq() {
    uint8_t status = inb(0x64);
    if ((status & 0x01) == 0) {
        return;
    }

    uint8_t scancode = inb(0x60);

    if (scancode == 0xE0 || scancode == 0xE1) {
        return;
    }

    if (scancode & 0x80) {
        uint8_t code = static_cast<uint8_t>(scancode & 0x7F);
        if (code == 0x2A || code == 0x36) {
            if (code == 0x2A) {
                g_left_shift_down = false;
            } else {
                g_right_shift_down = false;
            }
            update_shift_state();
        }
        return;
    }

    switch (scancode) {
        case 0x2A:  // Left Shift
        case 0x36:  // Right Shift
            if (scancode == 0x2A) {
                g_left_shift_down = true;
            } else {
                g_right_shift_down = true;
            }
            update_shift_state();
            return;
        case 0x3A:  // Caps Lock
            g_caps_lock = !g_caps_lock;
            return;
        default:
            break;
    }

    if (scancode >= sizeof(kScancodeMap)) {
        return;
    }

    char base = kScancodeMap[scancode];
    if (base == 0) {
        return;
    }

    char ch = base;
    bool alphabetic = (ch >= 'a' && ch <= 'z');
    if (alphabetic) {
        bool upper = g_shift ^ g_caps_lock;
        if (upper) {
            ch = static_cast<char>(ch - 'a' + 'A');
        }
    } else if (g_shift) {
        char shifted = kScancodeShiftMap[scancode];
        if (shifted != 0) {
            ch = shifted;
        }
    }

    enqueue(ch);
}

size_t read(char* buffer, size_t max_length) {
    if (buffer == nullptr || max_length == 0) {
        return 0;
    }

    size_t count = 0;
    while (count < max_length) {
        char ch;
        if (!dequeue(ch)) {
            break;
        }
        buffer[count++] = ch;
    }
    return count;
}

void inject_char(char ch) {
    enqueue(ch);
}

}  // namespace keyboard
