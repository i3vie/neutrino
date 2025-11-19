#include "mem.hpp"

extern "C" void *memcpy(void *dest, const void *src, size_t n) {
    uint8_t *d = (uint8_t*)dest;
    const uint8_t *s = (const uint8_t*)src;
    for (size_t i = 0; i < n; i++)
        d[i] = s[i];
    return dest;
}

extern "C" void *memmove(void *dest, const void *src, size_t n) {
    uint8_t *d = (uint8_t*)dest;
    const uint8_t *s = (const uint8_t*)src;
    if (d < s) {
        for (size_t i = 0; i < n; i++)
            d[i] = s[i];
    } else {
        for (size_t i = n; i != 0; i--)
            d[i-1] = s[i-1];
    }
    return dest;
}

extern "C" void *memset(void *s, int c, size_t n) {
    uint8_t *p = (uint8_t*)s;
    for (size_t i = 0; i < n; i++)
        p[i] = (uint8_t)c;
    return s;
}

extern "C" int memcmp(const void *s1, const void *s2, size_t n) {
    const uint8_t *a = (const uint8_t*)s1;
    const uint8_t *b = (const uint8_t*)s2;
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i])
            return (int)a[i] - (int)b[i];
    }
    return 0;
}

namespace {

constexpr size_t kWordSize = sizeof(uint64_t);

inline void copy_forward_align(uint8_t*& dst,
                               const uint8_t*& src,
                               size_t& remaining) {
    while (remaining != 0 &&
           (reinterpret_cast<uintptr_t>(dst) & (kWordSize - 1)) != 0) {
        *dst++ = *src++;
        --remaining;
    }
}

inline void copy_backward_align(uint8_t*& dst,
                                const uint8_t*& src,
                                size_t& remaining) {
    while (remaining != 0 &&
           (reinterpret_cast<uintptr_t>(dst) & (kWordSize - 1)) != 0) {
        --dst;
        --src;
        *dst = *src;
        --remaining;
    }
}

}  // namespace

extern "C" void *memcpy_fast(void *dest, const void *src, size_t n) {
    if (n == 0 || dest == src) {
        return dest;
    }

    if (n < 32) {
        return memcpy(dest, src, n);
    }

    auto* d = static_cast<uint8_t*>(dest);
    auto* s = static_cast<const uint8_t*>(src);
    size_t remaining = n;

    copy_forward_align(d, s, remaining);

    while (remaining >= kWordSize * 4) {
        auto* dst64 = reinterpret_cast<uint64_t*>(d);
        auto* src64 = reinterpret_cast<const uint64_t*>(s);
        dst64[0] = src64[0];
        dst64[1] = src64[1];
        dst64[2] = src64[2];
        dst64[3] = src64[3];
        d += kWordSize * 4;
        s += kWordSize * 4;
        remaining -= kWordSize * 4;
    }

    while (remaining >= kWordSize) {
        *reinterpret_cast<uint64_t*>(d) = *reinterpret_cast<const uint64_t*>(s);
        d += kWordSize;
        s += kWordSize;
        remaining -= kWordSize;
    }

    while (remaining != 0) {
        *d++ = *s++;
        --remaining;
    }

    return dest;
}

extern "C" void *memmove_fast(void *dest, const void *src, size_t n) {
    if (n == 0 || dest == src) {
        return dest;
    }

    auto* d = static_cast<uint8_t*>(dest);
    auto* s = static_cast<const uint8_t*>(src);

    if (d < s) {
        return memcpy_fast(dest, src, n);
    }

    if (n < 32) {
        return memmove(dest, src, n);
    }

    size_t remaining = n;
    auto* d_end = d + n;
    auto* s_end = s + n;

    copy_backward_align(d_end, s_end, remaining);

    while (remaining >= kWordSize * 4) {
        d_end -= kWordSize * 4;
        s_end -= kWordSize * 4;
        auto* dst64 = reinterpret_cast<uint64_t*>(d_end);
        auto* src64 = reinterpret_cast<const uint64_t*>(s_end);
        dst64[3] = src64[3];
        dst64[2] = src64[2];
        dst64[1] = src64[1];
        dst64[0] = src64[0];
        remaining -= kWordSize * 4;
    }

    while (remaining >= kWordSize) {
        d_end -= kWordSize;
        s_end -= kWordSize;
        *reinterpret_cast<uint64_t*>(d_end) =
            *reinterpret_cast<const uint64_t*>(s_end);
        remaining -= kWordSize;
    }

    while (remaining != 0) {
        --d_end;
        --s_end;
        *d_end = *s_end;
        --remaining;
    }

    return dest;
}
