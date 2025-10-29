// build with:
// g++ -ffreestanding -fno-builtin -fno-stack-protector -nostdlib -m64 -fPIE -pie -I../crt ../build/crt0.o shell.cpp -o shell.elf

#include "../crt/syscall.hpp"
#include <stdint.h>

#define DESC_TYPE_CONSOLE  1 // In the future, these would ideally be in
#define DESC_TYPE_KEYBOARD 3 // kernel-provided headers

int main(void) {
    long console = descriptor_open(DESC_TYPE_CONSOLE, 0);
    if (console < 0) return 1;

    long keyboard = descriptor_open(DESC_TYPE_KEYBOARD, 0);
    if (keyboard < 0) return 1;

    const char prompt[] = "> ";
    const char initial_prompt[] = "\n> ";
    descriptor_write(console, initial_prompt, sizeof(initial_prompt) - 1);

    uint8_t key;
    while (1) {
        long r = descriptor_read(keyboard, &key, 1);
        if (r > 0) {
            descriptor_write(console, &key, (size_t)r);
            if (key == '\n' || key == '\r')
                descriptor_write(console, prompt, sizeof(prompt) - 1);
        }
        yield();
    }

    return 0;
}
