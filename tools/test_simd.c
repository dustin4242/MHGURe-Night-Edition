// Differential test of the AVX2 kernels (runtime/avx2_kernels.c) against the baseline ones they replace
// (runtime/simd_kernels.c): random inputs, byte for byte, over a spread of sizes and alignments. No game files
// needed: `make test-simd`. Exits non-zero on a mismatch.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host_cpu.h"
#include "simd_kernels.h"

static uint64_t rng = 0x243F6A8885A308D3ull;
static uint32_t rnd(void) {
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (uint32_t)(rng >> 32);
}

static int failures;

static void check_rgb24_rgba32(size_t pixels) {
    const size_t in = pixels * 3 + 16, out = pixels * 4 + 16;
    uint8_t *src = malloc(in), *base = malloc(out), *avx2 = malloc(out);
    for (size_t i = 0; i < in; i++) src[i] = (uint8_t)rnd();
    // both outputs start at the same poison, so a kernel that writes past the end (or not at all) shows up too
    memset(base, 0xA5, out);
    memset(avx2, 0xA5, out);
    sk_rgb24_rgba32_base(src, base, pixels);
    sk_rgb24_rgba32_avx2(src, avx2, pixels);
    if (memcmp(base, avx2, out)) {
        printf("FAIL rgb24->rgba32: %zu pixels differ\n", pixels);
        failures++;
    } else {
        for (size_t i = 0; i < pixels; i++) {
            if (avx2[i * 4 + 3] != 255) {
                printf("FAIL rgb24->rgba32: alpha not set at %zu pixels\n", pixels);
                failures++;
                break;
            }
        }
    }
    free(src);
    free(base);
    free(avx2);
}

static void check_any_gt_u32(size_t n, uint32_t x) {
    uint32_t *v = malloc((n + 1) * sizeof *v);
    for (size_t i = 0; i < n; i++) v[i] = rnd();
    int base = sk_any_gt_u32_base(v, n, x), avx2 = sk_any_gt_u32_avx2(v, n, x);
    if (base != avx2) { printf("FAIL any_gt_u32: %zu words, x=%u: %d vs %d\n", n, x, base, avx2); failures++; }
    // one word over the limit must be found wherever it sits (the vector loop must not skip a block's remainder)
    for (size_t at = 0; x != 0xFFFFFFFFu && at < n; at++) {
        for (size_t i = 0; i < n; i++) v[i] = x;  // all equal: not greater
        v[at] = x + 1;
        if (sk_any_gt_u32_avx2(v, n, x) != 1) { printf("FAIL any_gt_u32: miss at %zu of %zu\n", at, n); failures++; }
    }
    free(v);
}

int main(void) {
    host_cpu_init();
    printf("cpu: sse4.2 %s, avx2 %s\n", host_cpu_sse42() ? "yes" : "no", host_cpu_avx2() ? "yes" : "no");
    if (!host_cpu_avx2()) {
        printf("no AVX2 on this CPU: the baseline kernels are the only ones that can run here\n");
        return 0;
    }
    // every small size (the loop boundary and the scalar tail), then a spread of larger ones
    for (size_t n = 0; n <= 40; n++) check_rgb24_rgba32(n);
    for (int i = 0; i < 300; i++) check_rgb24_rgba32(1 + rnd() % 5000);
    check_rgb24_rgba32(1u << 20);  // a whole 1024x1024 RGB8 texture
    for (size_t n = 0; n <= 40; n++) {
        check_any_gt_u32(n, 0);
        check_any_gt_u32(n, 7);
        check_any_gt_u32(n, 0xFFFFFFFFu);  // nothing is greater than this: both must say no
    }
    for (int i = 0; i < 100; i++) check_any_gt_u32(1 + rnd() % 300, rnd());
    printf("%s\n", failures ? "FAILED" : "ok: the AVX2 kernels match the baseline byte for byte");
    return failures != 0;
}
