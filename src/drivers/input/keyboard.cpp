#include "keyboard.hpp"

#include "arch/x86_64/io.hpp"
#include "../interrupts/pic.hpp"
#include "../log/logging.hpp"
#include "../../kernel/descriptor.hpp"

namespace keyboard {
namespace {

constexpr size_t kBufferSize = 256;
constexpr size_t kInputSlots = 6;

struct SlotBuffer {
    char data[kBufferSize];
    size_t head;
    size_t tail;
};

SlotBuffer g_buffers[kInputSlots];

bool g_shift = false;
bool g_caps_lock = false;
bool g_initialized = false;
bool g_left_shift_down = false;
bool g_right_shift_down = false;
bool g_left_ctrl_down = false;
bool g_left_alt_down = false;

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

void enqueue(uint32_t slot, char ch) {
    if (slot >= kInputSlots) {
        return;
    }
    SlotBuffer& buf = g_buffers[slot];
    size_t next = (buf.head + 1) % kBufferSize;
    if (next == buf.tail) {
        return;
    }
    buf.data[buf.head] = ch;
    buf.head = next;
}

bool dequeue(uint32_t slot, char& ch) {
    if (slot >= kInputSlots) {
        return false;
    }
    SlotBuffer& buf = g_buffers[slot];
    if (buf.head == buf.tail) {
        return false;
    }
    ch = buf.data[buf.tail];
    buf.tail = (buf.tail + 1) % kBufferSize;
    return true;
}

void update_shift_state() {
    g_shift = g_left_shift_down || g_right_shift_down;
}

void update_modifier_state() {
    update_shift_state();
}

}  // namespace

void init() {
    if (g_initialized) {
        return;
    }
    for (auto& buf : g_buffers) {
        buf.head = 0;
        buf.tail = 0;
    }
    g_shift = false;
    g_caps_lock = false;
    g_left_shift_down = false;
    g_right_shift_down = false;
    g_left_ctrl_down = false;
    g_left_alt_down = false;
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
        if (code == 0x2A || code == 0x36 ||
            code == 0x1D || code == 0x38) {
            if (code == 0x2A) {
                g_left_shift_down = false;
            } else if (code == 0x36) {
                g_right_shift_down = false;
            } else if (code == 0x1D) {
                g_left_ctrl_down = false;
            } else if (code == 0x38) {
                g_left_alt_down = false;
            }
            update_modifier_state();
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
            update_modifier_state();
            return;
        case 0x1D:  // Left Control
            g_left_ctrl_down = true;
            update_modifier_state();
            return;
        case 0x38:  // Left Alt
            g_left_alt_down = true;
            update_modifier_state();
            return;
        case 0x3A:  // Caps Lock
            g_caps_lock = !g_caps_lock;
            return;
        default:
            break;
    }

    if (g_left_ctrl_down && g_left_alt_down && g_shift) {
        if (scancode >= 0x3B && scancode <= 0x40) {
            uint32_t index = static_cast<uint32_t>(scancode - 0x3B);
            descriptor::framebuffer_select(index);
            return;
        }
    }
    if (g_left_ctrl_down && g_shift) {
        if (scancode >= 0x02 && scancode <= 0x07) {
            uint32_t index = static_cast<uint32_t>(scancode - 0x02);
            descriptor::framebuffer_select(index);
            return;
        }
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

    uint32_t slot = descriptor::framebuffer_active_slot();
    if (slot >= kInputSlots) {
        slot = 0;
    }
    enqueue(slot, ch);
}

size_t read(uint32_t slot, char* buffer, size_t max_length) {
    if (buffer == nullptr || max_length == 0) {
        return 0;
    }
    if (slot >= kInputSlots) {
        return 0;
    }

    size_t count = 0;
    while (count < max_length) {
        char ch;
        if (!dequeue(slot, ch)) {
            break;
        }
        buffer[count++] = ch;
    }
    return count;
}

void inject_char(char ch) {
    uint32_t slot = descriptor::framebuffer_active_slot();
    if (slot >= kInputSlots) {
        slot = 0;
    }
    enqueue(slot, ch);
}

}  // namespace keyboard
