// What the host CPU can do (runtime/host_cpu.c). The port is built for the x86-64-v2 baseline (SSE4.2, no AVX), so
// instruction sets above it live in their own files, compiled for that set and reached only through the pointers
// simd_kernels_init() sets here: a machine without AVX2 never runs a byte of that code.
#pragma once

#include <stdbool.h>

// Reads CPUID (and XCR0, so that "the OS saves the YMM state" is part of the answer) and picks the kernels.
// MHGU_NO_AVX2=1 (or anything but "0"/empty) forces the baseline, to exercise the fallback path on a fast machine.
void host_cpu_init(void);

bool host_cpu_avx2(void);
bool host_cpu_sse42(void);

// "avx2" or "baseline": what the port ended up using, for log lines and the overlay.
const char *host_cpu_isa(void);
