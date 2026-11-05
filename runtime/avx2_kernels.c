// The AVX2 versions of the loops in runtime/simd_kernels.h. This is the only file the build compiles with -mavx2
// (Makefile), and every function in it is reached through a pointer that host_cpu_init() sets only when the CPU has
// AVX2 and the OS saves the YMM state -- so on any other machine not one byte of this file is entered.
#include <stdint.h>

#include "simd_kernels.h"

// x86-64 only: on other targets the Makefile leaves out -mavx2, and this file only provides the symbols (nothing
// selects them there: host_cpu_init never turns the AVX2 kernels on).
#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>

// 8 pixels per iteration: two 128-bit lanes, each unpacking 4 RGB triples into 4 RGBA texels with one pshufb, then
// the alpha bytes are ORed in (the shuffle leaves them 0). The 24-byte input stride means the loads are unaligned;
// the loop stops 12 pixels short of the end so that the second load, which reads 16 bytes to use 12, stays inside
// the buffer, and the tail runs the scalar loop.
void sk_rgb24_rgba32_avx2(const uint8_t *src, uint8_t *dst, size_t pixels) {
    const __m256i shuf = _mm256_setr_epi8(0, 1, 2, -1, 3, 4, 5, -1, 6, 7, 8, -1, 9, 10, 11, -1,
                                          0, 1, 2, -1, 3, 4, 5, -1, 6, 7, 8, -1, 9, 10, 11, -1);
    const __m256i alpha = _mm256_set1_epi32((int)0xFF000000u);
    size_t i = 0;
    for (; i + 12 <= pixels; i += 8) {
        __m256i in = _mm256_set_m128i(_mm_loadu_si128((const __m128i *)(src + i * 3 + 12)),
                                      _mm_loadu_si128((const __m128i *)(src + i * 3)));
        _mm256_storeu_si256((__m256i *)(dst + i * 4), _mm256_or_si256(_mm256_shuffle_epi8(in, shuf), alpha));
    }
    for (; i < pixels; i++) {
        dst[i * 4] = src[i * 3];
        dst[i * 4 + 1] = src[i * 3 + 1];
        dst[i * 4 + 2] = src[i * 3 + 2];
        dst[i * 4 + 3] = 255;
    }
}

// 8 words per iteration, unsigned comparison done by flipping the sign bit (cmpgt is signed). Returns as soon as a
// block has a match, like the plain loop.
int sk_any_gt_u32_avx2(const uint32_t *v, size_t n, uint32_t x) {
    const __m256i sign = _mm256_set1_epi32((int)0x80000000u);
    const __m256i xv = _mm256_xor_si256(_mm256_set1_epi32((int)x), sign);
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        __m256i a = _mm256_xor_si256(_mm256_loadu_si256((const __m256i *)(v + i)), sign);
        if (_mm256_movemask_epi8(_mm256_cmpgt_epi32(a, xv))) return 1;
    }
    for (; i < n; i++)
        if (v[i] > x) return 1;
    return 0;
}

#else  // not x86-64: the baseline versions, so the symbols exist for the kernel table

void sk_rgb24_rgba32_avx2(const uint8_t *src, uint8_t *dst, size_t pixels) { sk_rgb24_rgba32_base(src, dst, pixels); }
int sk_any_gt_u32_avx2(const uint32_t *v, size_t n, uint32_t x) { return sk_any_gt_u32_base(v, n, x); }

#endif
