// The baseline (x86-64-v2) versions of the kernels in runtime/simd_kernels.h, and the table that picks between them
// and the AVX2 ones. Everything the port ships is compiled for this baseline, so these are the ones that always run
// on a machine without AVX2 -- and the reference the AVX2 versions are tested against (make test-simd).
#include "simd_kernels.h"

void (*sk_rgb24_rgba32)(const uint8_t *src, uint8_t *dst, size_t pixels) = sk_rgb24_rgba32_base;
int (*sk_any_gt_u32)(const uint32_t *v, size_t n, uint32_t x) = sk_any_gt_u32_base;

void sk_rgb24_rgba32_base(const uint8_t *src, uint8_t *dst, size_t pixels) {
    for (size_t i = 0; i < pixels; i++) {
        dst[i * 4] = src[i * 3];
        dst[i * 4 + 1] = src[i * 3 + 1];
        dst[i * 4 + 2] = src[i * 3 + 2];
        dst[i * 4 + 3] = 255;
    }
}

int sk_any_gt_u32_base(const uint32_t *v, size_t n, uint32_t x) {
    for (size_t i = 0; i < n; i++)
        if (v[i] > x) return 1;
    return 0;
}

void simd_kernels_init(bool avx2) {
    sk_rgb24_rgba32 = avx2 ? sk_rgb24_rgba32_avx2 : sk_rgb24_rgba32_base;
    sk_any_gt_u32 = avx2 ? sk_any_gt_u32_avx2 : sk_any_gt_u32_base;
}
