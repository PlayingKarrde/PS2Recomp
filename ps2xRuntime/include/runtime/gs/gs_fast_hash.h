#pragma once

// A fast 64-bit hash for the GPU renderer's texture cache (task g5).
//
// The cache finds a texture again by hashing the bytes of the GS memory pages it lives in. On the idle wrestler select
// screen the game re-uploads the same data every frame, so about 75 lookups and 6 MB of pages are hashed per frame, and
// with XXH64 (GSRec::Hash64, 3.9 GB/s here) that was 1.5 ms of the game thread's frame. This is the long-input loop of
// XXH3 in its SSE2 form (Yann Collet's xxHash, BSD-2-Clause; the idea and the multiply-and-swap accumulation are his): four
// 128-bit accumulators take a 64-byte stripe per step, each lane adds the stripe word it did not read (so every word
// counts twice) and the product of the two halves of (word xor secret), and the secret slides by one word per stripe so
// two stripes swapped give another hash. A portable version computes exactly the same value, so the result does not
// depend on the CPU. It is not XXH3: the tail and the final mix are simpler, and the value is only ever used as a key in
// one process, never stored.
//
// Not a cryptographic hash. A collision would show a stale texture; the cache already trusts 64 bits (XXH64) for that.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <emmintrin.h>
#define PS2X_FAST_HASH_SSE2 1
#endif

namespace GSFastHash
{
    // The fixed secret (at least 184 bytes are read): the first bytes of the XXH3 default secret, then the same again.
    inline const uint8_t *Secret()
    {
        alignas(16) static const uint8_t secret[] = {
            0xb8, 0xfe, 0x6c, 0x39, 0x23, 0xa4, 0x4b, 0xbe, 0x7c, 0x01, 0x81, 0x2c, 0xf7, 0x21, 0xad, 0x1c, 0xde, 0xd4, 0x6d, 0xe9, 0x83, 0x90, 0x97, 0xdb,
            0x72, 0x40, 0xa4, 0xa4, 0xb7, 0xb3, 0x67, 0x1f, 0xcb, 0x79, 0xe6, 0x4e, 0xcc, 0xc0, 0xe5, 0x78, 0x82, 0x5a, 0xd0, 0x7d, 0xcc, 0xff, 0x72, 0x21,
            0xb8, 0x08, 0x46, 0x74, 0xf7, 0x43, 0x24, 0x8e, 0xe0, 0x35, 0x90, 0xe6, 0x81, 0x3a, 0x26, 0x4c, 0x3c, 0x28, 0x52, 0xbb, 0x91, 0xc3, 0x00, 0xcb,
            0x88, 0xd0, 0x65, 0x8b, 0x1b, 0x53, 0x2e, 0xa3, 0x71, 0x64, 0x48, 0x97, 0xa2, 0x0d, 0xf9, 0x4e, 0x38, 0x19, 0xef, 0x46, 0xa9, 0xde, 0xac, 0xd8,
            0xa8, 0xfa, 0x76, 0x3f, 0xe3, 0x9c, 0x34, 0x3f, 0xf9, 0xdc, 0xbb, 0xc7, 0xc7, 0x0b, 0x4f, 0x1d, 0x8a, 0x51, 0xe0, 0x4b, 0xcd, 0xb4, 0x59, 0x31,
            0xc8, 0x9f, 0x7e, 0xc9, 0xd9, 0x78, 0x73, 0x64, 0xea, 0xc5, 0xac, 0x83, 0x34, 0xd3, 0xeb, 0xc3, 0xc5, 0x81, 0xa0, 0xff, 0xfa, 0x13, 0x63, 0xeb,
            0x17, 0x0d, 0xdd, 0x51, 0xb7, 0xf0, 0xda, 0x49, 0xd3, 0x16, 0x55, 0x26, 0x29, 0xd4, 0x68, 0x9e, 0x2b, 0x16, 0xbe, 0x58, 0x7d, 0x47, 0xa1, 0xfc,
            0x8f, 0xf8, 0xb8, 0xd1, 0x7a, 0xd0, 0x31, 0xce, 0x45, 0xcb, 0x3a, 0x8f, 0x95, 0x16, 0x04, 0x28, 0xaf, 0xd7, 0xfb, 0xca, 0xbb, 0x4b, 0x40, 0x7e,
            0xb8, 0xfe, 0x6c, 0x39, 0x23, 0xa4, 0x4b, 0xbe, 0x7c, 0x01, 0x81, 0x2c, 0xf7, 0x21, 0xad, 0x1c, 0xde, 0xd4, 0x6d, 0xe9, 0x83, 0x90, 0x97, 0xdb,
            0x72, 0x40, 0xa4, 0xa4, 0xb7, 0xb3, 0x67, 0x1f, 0xcb, 0x79, 0xe6, 0x4e, 0xcc, 0xc0, 0xe5, 0x78, 0x82, 0x5a, 0xd0, 0x7d, 0xcc, 0xff, 0x72, 0x21,
            0xb8, 0x08, 0x46, 0x74, 0xf7, 0x43, 0x24, 0x8e, 0xe0, 0x35, 0x90, 0xe6, 0x81, 0x3a, 0x26, 0x4c, 0x3c, 0x28, 0x52, 0xbb, 0x91, 0xc3, 0x00, 0xcb,
            0x88, 0xd0, 0x65, 0x8b, 0x1b, 0x53, 0x2e, 0xa3, 0x71, 0x64, 0x48, 0x97, 0xa2, 0x0d, 0xf9, 0x4e, 0x38, 0x19, 0xef, 0x46, 0xa9, 0xde, 0xac, 0xd8,
            0xa8, 0xfa, 0x76, 0x3f, 0xe3, 0x9c, 0x34, 0x3f, 0xf9, 0xdc, 0xbb, 0xc7, 0xc7, 0x0b, 0x4f, 0x1d, 0x8a, 0x51, 0xe0, 0x4b, 0xcd, 0xb4, 0x59, 0x31,
            0xc8, 0x9f, 0x7e, 0xc9, 0xd9, 0x78, 0x73, 0x64, 0xea, 0xc5, 0xac, 0x83, 0x34, 0xd3, 0xeb, 0xc3, 0xc5, 0x81, 0xa0, 0xff, 0xfa, 0x13, 0x63, 0xeb,
            0x17, 0x0d, 0xdd, 0x51, 0xb7, 0xf0, 0xda, 0x49, 0xd3, 0x16, 0x55, 0x26, 0x29, 0xd4, 0x68, 0x9e, 0x2b, 0x16, 0xbe, 0x58, 0x7d, 0x47, 0xa1, 0xfc,
            0x8f, 0xf8, 0xb8, 0xd1, 0x7a, 0xd0, 0x31, 0xce, 0x45, 0xcb, 0x3a, 0x8f, 0x95, 0x16, 0x04, 0x28, 0xaf, 0xd7, 0xfb, 0xca, 0xbb, 0x4b, 0x40, 0x7e};
        static_assert(sizeof(secret) >= 15 * 8 + 64, "the secret slides by up to 15 words and a stripe reads 64 bytes");
        return secret;
    }

    constexpr uint64_t kPrime1 = 0x9E3779B185EBCA87ull;
    constexpr uint64_t kPrime2 = 0xC2B2AE3D27D4EB4Full;
    constexpr uint64_t kPrime3 = 0x165667B19E3779F9ull;

    inline uint64_t Load64(const uint8_t *p)
    {
        uint64_t v;
        std::memcpy(&v, p, sizeof(v));
        return v;
    }

    // The accumulate step for the 64-byte stripe at `p` with the secret at `secret`; `acc` is 8 words.
    inline void AccumulatePortable(uint64_t *acc, const uint8_t *p, const uint8_t *secret)
    {
        for (int i = 0; i < 8; ++i)
        {
            const uint64_t data = Load64(p + 8 * i);
            const uint64_t key = data ^ Load64(secret + 8 * i);
            acc[i ^ 1] += data;
            acc[i] += (key & 0xFFFFFFFFull) * (key >> 32);
        }
    }

#if defined(PS2X_FAST_HASH_SSE2)
    inline void AccumulateSse2(__m128i *acc, const uint8_t *p, const uint8_t *secret)
    {
        for (int i = 0; i < 4; ++i)
        {
            const __m128i data = _mm_loadu_si128(reinterpret_cast<const __m128i *>(p + 16 * i));
            const __m128i key = _mm_xor_si128(data, _mm_loadu_si128(reinterpret_cast<const __m128i *>(secret + 16 * i)));
            const __m128i keyHi = _mm_shuffle_epi32(key, _MM_SHUFFLE(0, 3, 0, 1));
            const __m128i product = _mm_mul_epu32(key, keyHi);
            const __m128i swapped = _mm_shuffle_epi32(data, _MM_SHUFFLE(1, 0, 3, 2));
            acc[i] = _mm_add_epi64(_mm_add_epi64(acc[i], swapped), product);
        }
    }
#endif

    inline uint64_t Mix(uint64_t x)
    {
        x ^= x >> 33;
        x *= kPrime2;
        x ^= x >> 29;
        x *= kPrime3;
        x ^= x >> 32;
        return x;
    }

    // After every 16 stripes (1 KiB) the accumulators are scrambled, which makes the order of stripes that are a multiple
    // of 16 apart matter (they use the same part of the secret, and the accumulation alone is commutative).
    inline void Scramble(uint64_t *acc, const uint8_t *secret)
    {
        for (int i = 0; i < 8; ++i)
        {
            uint64_t a = acc[i];
            a ^= a >> 47;
            a ^= Load64(secret + 8 * i + 128);
            acc[i] = a * kPrime1;
        }
    }

    // `portable` forces the portable loop (the unit test compares the two).
    inline uint64_t Hash(const void *data, size_t len, uint64_t seed = 0, bool portable = false)
    {
        const uint8_t *p = static_cast<const uint8_t *>(data);
        const uint8_t *const secret = Secret();
        uint64_t acc[8] = {seed + kPrime3, kPrime1, kPrime2, seed - kPrime1, seed + kPrime2, kPrime3, kPrime1 + seed, ~seed};
        size_t stripe = 0;
        const size_t stripes = len / 64u;
        while (stripe < stripes)
        {
            const size_t inBlock = std::min<size_t>(16u - (stripe & 15u), stripes - stripe);
#if defined(PS2X_FAST_HASH_SSE2)
            if (!portable)
            {
                __m128i a[4];
                for (int i = 0; i < 4; ++i)
                    a[i] = _mm_loadu_si128(reinterpret_cast<const __m128i *>(acc + 2 * i));
                for (size_t k = 0; k < inBlock; ++k, p += 64)
                    AccumulateSse2(a, p, secret + ((stripe + k) & 15u) * 8u);
                for (int i = 0; i < 4; ++i)
                    _mm_storeu_si128(reinterpret_cast<__m128i *>(acc + 2 * i), a[i]);
            }
            else
#endif
            {
                for (size_t k = 0; k < inBlock; ++k, p += 64)
                    AccumulatePortable(acc, p, secret + ((stripe + k) & 15u) * 8u);
            }
            stripe += inBlock;
            if ((stripe & 15u) == 0u)
                Scramble(acc, secret);
        }
        // The last 0..63 bytes: zero-padded into one more stripe, so the tail length matters through `len` below.
        if (len & 63u)
        {
            uint8_t last[64] = {};
            std::memcpy(last, p, len & 63u);
            AccumulatePortable(acc, last, secret + (stripe & 15u) * 8u);
        }
        uint64_t h = static_cast<uint64_t>(len) * kPrime1 ^ seed;
        for (int i = 0; i < 8; ++i)
            h = Mix(h ^ acc[i]) + acc[i] * kPrime1 + static_cast<uint64_t>(i);
        return Mix(h);
    }
} // namespace GSFastHash
