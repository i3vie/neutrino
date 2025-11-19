#pragma once
#include <stddef.h>
#include <stdint.h>

#include "lib/font8x8_basic.hpp"
#include "kernel/descriptor.hpp"

constexpr int scale = 2;

struct Framebuffer {
    uint8_t* base;
    size_t width;
    size_t height;
    size_t pitch;
    uint16_t bpp;
    uint8_t memory_model;
    uint8_t red_mask_size;
    uint8_t red_mask_shift;
    uint8_t green_mask_size;
    uint8_t green_mask_shift;
    uint8_t blue_mask_size;
    uint8_t blue_mask_shift;
};

class Console {
public:
    explicit Console(uint32_t framebuffer_handle);

    void putc(char c);
    void puts(const char* s);
    void printf(const char* fmt, ...);
    void clear();

    void set_color(uint32_t fg, uint32_t bg);

    bool enable_back_buffer();

private:
    uint32_t framebuffer_handle;
    Framebuffer primary_fb;
    size_t cursor_x;
    size_t cursor_y;
    uint32_t fg_color;
    uint32_t bg_color;
    size_t columns;
    size_t rows;
    size_t text_width;
    size_t text_height;
    Framebuffer back_fb;
    uint8_t* back_buffer;
    size_t frame_bytes;
    size_t back_buffer_capacity;

    bool refresh_framebuffer_info();
    bool allocate_back_buffer();
    Framebuffer* draw_target();
    void flush_region(size_t x, size_t y, size_t width, size_t height);
    void flush_all();

    void draw_char(char c, size_t x, size_t y);
    void scroll();
    void print_dec(uint64_t n);
    void print_hex(uint64_t n, bool pad16);
};

extern Console* kconsole;
