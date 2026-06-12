#include <stddef.h>
#include <stdint.h>

#include "string.h"

namespace {

constexpr size_t kWordSize = sizeof(uint64_t);
constexpr size_t kMemcpyByteThreshold = 32;

inline void* memcpy_bytewise(void* dest, const void* src, size_t count) {
    auto* out = static_cast<uint8_t*>(dest);
    auto* in = static_cast<const uint8_t*>(src);
    for (size_t i = 0; i < count; ++i) {
        out[i] = in[i];
    }
    return dest;
}

inline void copy_forward_align(uint8_t*& dest,
                               const uint8_t*& src,
                               size_t& remaining) {
    while (remaining != 0 &&
           (reinterpret_cast<uintptr_t>(dest) & (kWordSize - 1)) != 0) {
        *dest++ = *src++;
        --remaining;
    }
}

}  // namespace

extern "C" void* memcpy(void* dest, const void* src, size_t count) {
    if (count == 0 || dest == src) {
        return dest;
    }

    if (count < kMemcpyByteThreshold) {
        return memcpy_bytewise(dest, src, count);
    }

    auto* out = static_cast<uint8_t*>(dest);
    auto* in = static_cast<const uint8_t*>(src);
    size_t remaining = count;

    copy_forward_align(out, in, remaining);

    while (remaining >= kWordSize * 4) {
        auto* out64 = reinterpret_cast<uint64_t*>(out);
        auto* in64 = reinterpret_cast<const uint64_t*>(in);
        out64[0] = in64[0];
        out64[1] = in64[1];
        out64[2] = in64[2];
        out64[3] = in64[3];
        out += kWordSize * 4;
        in += kWordSize * 4;
        remaining -= kWordSize * 4;
    }

    while (remaining >= kWordSize) {
        *reinterpret_cast<uint64_t*>(out) =
            *reinterpret_cast<const uint64_t*>(in);
        out += kWordSize;
        in += kWordSize;
        remaining -= kWordSize;
    }

    while (remaining != 0) {
        *out++ = *in++;
        --remaining;
    }

    return dest;
}

extern "C" void* memset(void* dest, int value, size_t count) {
    auto* out = static_cast<uint8_t*>(dest);
    uint8_t byte = static_cast<uint8_t>(value);

    for (size_t i = 0; i < count; ++i) {
        out[i] = byte;
    }

    return dest;
}

extern "C" void* memmove(void* dest, const void* src, size_t count) {
    auto* out = static_cast<uint8_t*>(dest);
    auto* in = static_cast<const uint8_t*>(src);

    if (out == in || count == 0) {
        return dest;
    }

    if (out < in) {
        return memcpy(dest, src, count);
    }

    for (size_t i = count; i != 0; --i) {
        out[i - 1] = in[i - 1];
    }
    return dest;
}

extern "C" int memcmp(const void* lhs, const void* rhs, size_t count) {
    auto* left = static_cast<const uint8_t*>(lhs);
    auto* right = static_cast<const uint8_t*>(rhs);

    for (size_t i = 0; i < count; ++i) {
        if (left[i] != right[i]) {
            return static_cast<int>(left[i]) - static_cast<int>(right[i]);
        }
    }

    return 0;
}

extern "C" int strcmp(const char* lhs, const char* rhs) {
    if (lhs == rhs) {
        return 0;
    }
    if (lhs == nullptr) {
        return -1;
    }
    if (rhs == nullptr) {
        return 1;
    }

    while (*lhs != '\0' && *rhs != '\0') {
        if (*lhs != *rhs) {
            return static_cast<unsigned char>(*lhs) -
                   static_cast<unsigned char>(*rhs);
        }
        ++lhs;
        ++rhs;
    }

    return static_cast<unsigned char>(*lhs) -
           static_cast<unsigned char>(*rhs);
}

extern "C" int strncmp(const char* lhs, const char* rhs, size_t count) {
    if (count == 0 || lhs == rhs) {
        return 0;
    }
    if (lhs == nullptr) {
        return -1;
    }
    if (rhs == nullptr) {
        return 1;
    }

    for (size_t i = 0; i < count; ++i) {
        if (lhs[i] != rhs[i]) {
            return static_cast<unsigned char>(lhs[i]) -
                   static_cast<unsigned char>(rhs[i]);
        }
        if (lhs[i] == '\0') {
            return 0;
        }
    }

    return 0;
}

extern "C" size_t strlen(const char* text) {
    if (text == nullptr) {
        return 0;
    }

    size_t length = 0;
    while (text[length] != '\0') {
        ++length;
    }
    return length;
}

extern "C" size_t strlcpy(char* dest, const char* src, size_t size) {
    size_t src_length = strlen(src);
    if (dest == nullptr || size == 0) {
        return src_length;
    }

    size_t copy_length = src_length;
    if (copy_length + 1 > size) {
        copy_length = size - 1;
    }

    if (copy_length != 0) {
        memcpy(dest, src, copy_length);
    }
    dest[copy_length] = '\0';
    return src_length;
}
