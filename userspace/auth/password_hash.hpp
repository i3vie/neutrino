#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "secure_random.hpp"

namespace auth {

constexpr uint32_t kPasswordAlgorithmPbkdf2Sha256 = 1;
constexpr uint32_t kPasswordIterations = 100000;
constexpr size_t kPasswordSaltSize = 16;
constexpr size_t kPasswordHashSize = 32;

struct Sha256 {
    uint8_t data[64];
    uint32_t datalen;
    uint64_t bitlen;
    uint32_t state[8];
};

inline uint32_t rotr(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32 - n));
}

inline uint32_t load_be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

inline void store_be32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}

inline void sha256_transform(Sha256& ctx, const uint8_t data[]) {
    static constexpr uint32_t k[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
        0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
        0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
        0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
        0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
        0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
        0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
        0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
        0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
        0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
        0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
        0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
    };
    uint32_t m[64];
    for (uint32_t i = 0; i < 16; ++i) {
        m[i] = load_be32(data + i * 4);
    }
    for (uint32_t i = 16; i < 64; ++i) {
        uint32_t s0 = rotr(m[i - 15], 7) ^ rotr(m[i - 15], 18) ^ (m[i - 15] >> 3);
        uint32_t s1 = rotr(m[i - 2], 17) ^ rotr(m[i - 2], 19) ^ (m[i - 2] >> 10);
        m[i] = m[i - 16] + s0 + m[i - 7] + s1;
    }
    uint32_t a = ctx.state[0], b = ctx.state[1], c = ctx.state[2], d = ctx.state[3];
    uint32_t e = ctx.state[4], f = ctx.state[5], g = ctx.state[6], h = ctx.state[7];
    for (uint32_t i = 0; i < 64; ++i) {
        uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t temp1 = h + s1 + ch + k[i] + m[i];
        uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }
    ctx.state[0] += a; ctx.state[1] += b; ctx.state[2] += c; ctx.state[3] += d;
    ctx.state[4] += e; ctx.state[5] += f; ctx.state[6] += g; ctx.state[7] += h;
}

inline void sha256_init(Sha256& ctx) {
    ctx.datalen = 0;
    ctx.bitlen = 0;
    ctx.state[0] = 0x6a09e667u; ctx.state[1] = 0xbb67ae85u;
    ctx.state[2] = 0x3c6ef372u; ctx.state[3] = 0xa54ff53au;
    ctx.state[4] = 0x510e527fu; ctx.state[5] = 0x9b05688cu;
    ctx.state[6] = 0x1f83d9abu; ctx.state[7] = 0x5be0cd19u;
}

inline void sha256_update(Sha256& ctx, const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        ctx.data[ctx.datalen++] = data[i];
        if (ctx.datalen == 64) {
            sha256_transform(ctx, ctx.data);
            ctx.bitlen += 512;
            ctx.datalen = 0;
        }
    }
}

inline void sha256_final(Sha256& ctx, uint8_t hash[32]) {
    uint32_t i = ctx.datalen;
    ctx.data[i++] = 0x80;
    if (i > 56) {
        while (i < 64) ctx.data[i++] = 0;
        sha256_transform(ctx, ctx.data);
        i = 0;
    }
    while (i < 56) ctx.data[i++] = 0;
    ctx.bitlen += ctx.datalen * 8;
    for (uint32_t j = 0; j < 8; ++j) {
        ctx.data[63 - j] = static_cast<uint8_t>(ctx.bitlen >> (j * 8));
    }
    sha256_transform(ctx, ctx.data);
    for (uint32_t j = 0; j < 8; ++j) {
        store_be32(hash + j * 4, ctx.state[j]);
    }
}

inline void hmac_sha256(const uint8_t* key,
                        size_t key_len,
                        const uint8_t* data,
                        size_t data_len,
                        uint8_t out[32]) {
    uint8_t key_block[64]{};
    if (key_len > sizeof(key_block)) {
        Sha256 kh;
        sha256_init(kh);
        sha256_update(kh, key, key_len);
        sha256_final(kh, key_block);
    } else if (key_len != 0) {
        memcpy(key_block, key, key_len);
    }
    uint8_t ipad[64];
    uint8_t opad[64];
    for (size_t i = 0; i < 64; ++i) {
        ipad[i] = key_block[i] ^ 0x36u;
        opad[i] = key_block[i] ^ 0x5cu;
    }
    uint8_t inner[32];
    Sha256 ctx;
    sha256_init(ctx);
    sha256_update(ctx, ipad, sizeof(ipad));
    sha256_update(ctx, data, data_len);
    sha256_final(ctx, inner);
    sha256_init(ctx);
    sha256_update(ctx, opad, sizeof(opad));
    sha256_update(ctx, inner, sizeof(inner));
    sha256_final(ctx, out);
}

inline void pbkdf2_sha256(const char* password,
                          const uint8_t* salt,
                          size_t salt_len,
                          uint32_t iterations,
                          uint8_t out[32]) {
    uint8_t block[64];
    if (salt_len > 60) {
        salt_len = 60;
    }
    memcpy(block, salt, salt_len);
    store_be32(block + salt_len, 1);
    uint8_t u[32];
    hmac_sha256(reinterpret_cast<const uint8_t*>(password),
                strlen(password),
                block,
                salt_len + 4,
                u);
    memcpy(out, u, 32);
    for (uint32_t i = 1; i < iterations; ++i) {
        hmac_sha256(reinterpret_cast<const uint8_t*>(password),
                    strlen(password),
                    u,
                    sizeof(u),
                    u);
        for (size_t j = 0; j < 32; ++j) {
            out[j] ^= u[j];
        }
    }
}

inline bool constant_time_equal(const uint8_t* a, const uint8_t* b, size_t len) {
    uint8_t diff = 0;
    for (size_t i = 0; i < len; ++i) {
        diff |= static_cast<uint8_t>(a[i] ^ b[i]);
    }
    return diff == 0;
}

inline bool make_salt(const char* name, uint8_t salt[kPasswordSaltSize]) {
    (void)name;
    return secure_random(salt, kPasswordSaltSize);
}

}  // namespace auth
