// Host CPU feature detection (runtime/host_cpu.h). Nothing else in the port asks the CPU what it can do: the build
// stays at x86-64-v2 so that one executable runs on everything from a 2009 Nehalem up, and the AVX2 kernels are
// picked here at startup.
#include "host_cpu.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "simd_kernels.h"

#if defined(_WIN32)
#include <intrin.h>
static void cpuid(uint32_t leaf, uint32_t sub, uint32_t out[4]) {
    int r[4];
    __cpuidex(r, (int)leaf, (int)sub);
    for (int i = 0; i < 4; i++) out[i] = (uint32_t)r[i];
}
static uint64_t xgetbv0(void) { return _xgetbv(0); }
#else
#include <cpuid.h>
static void cpuid(uint32_t leaf, uint32_t sub, uint32_t out[4]) {
    unsigned a, b, c, d;
    __cpuid_count(leaf, sub, a, b, c, d);
    out[0] = a; out[1] = b; out[2] = c; out[3] = d;
}
static uint64_t xgetbv0(void) {
    uint32_t lo, hi;
    __asm__ volatile(".byte 0x0f, 0x01, 0xd0" : "=a"(lo), "=d"(hi) : "c"(0));  // xgetbv, ecx = 0
    return (uint64_t)hi << 32 | lo;
}
#endif

static bool g_avx2, g_sse42;

// AVX2 needs three yeses: the CPU has it, the OS was asked to save the YMM state (XCR0), and the user has not
// turned it off. Any missing yes and the port runs the baseline kernels, which are always linked in.
static bool detect_avx2(void) {
    uint32_t r[4] = {0};
    cpuid(0, 0, r);
    uint32_t max_leaf = r[0];
    cpuid(1, 0, r);
    g_sse42 = (r[2] >> 20 & 1) != 0;
    bool osxsave = (r[2] >> 27 & 1) != 0, avx_hw = (r[2] >> 28 & 1) != 0;
    uint64_t xcr0 = osxsave ? xgetbv0() : 0;
    bool avx = avx_hw && (xcr0 & 6) == 6;  // SSE and YMM state both saved by the OS
    if (!avx || max_leaf < 7) return false;
    cpuid(7, 0, r);
    return (r[1] >> 5 & 1) != 0;  // CPUID.7.0:EBX[5] = AVX2
}

void host_cpu_init(void) {
    bool avx2 = detect_avx2();
    const char *off = getenv("MHGU_NO_AVX2");
    if (off && *off && strcmp(off, "0")) avx2 = false;
    g_avx2 = avx2;
    simd_kernels_init(avx2);
}

bool host_cpu_avx2(void) { return g_avx2; }
bool host_cpu_sse42(void) { return g_sse42; }
const char *host_cpu_isa(void) { return g_avx2 ? "avx2" : "baseline"; }
