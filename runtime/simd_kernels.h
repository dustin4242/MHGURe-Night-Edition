// Host-side loops the port can run in AVX2 (runtime/avx2_kernels.c) or in plain C (here). Both produce the same
// bytes, and simd_kernels_init() picks one set, so a call site never has to know which it got: call through the
// pointer, not the function.
//
// This is where "use AVX2 when the machine has it" happens. The recompiled game code is not part of it: it is built
// for the x86-64-v2 baseline like everything else (docs/AVX2-FALLBACK.md has the measurements).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Sprite and texture data in an RGB8 format: pixels texels of 3 bytes become pixels texels of 4, alpha set to 255
// (the upload path of textures in such a format: upload_texture() in runtime/nvn/vulkan/textures.c).
void sk_rgb24_rgba32_base(const uint8_t *src, uint8_t *dst, size_t pixels);
void sk_rgb24_rgba32_avx2(const uint8_t *src, uint8_t *dst, size_t pixels);

// Is any of n words greater than x (unsigned)? The per-frame check for "did the guest write to this texture's
// pages": vk_pages_changed() in runtime/nvn/vulkan/textures.c compares a texture's page generations against the one
// it saw last time. Same answer as a plain loop; only the scan is wider.
int sk_any_gt_u32_base(const uint32_t *v, size_t n, uint32_t x);
int sk_any_gt_u32_avx2(const uint32_t *v, size_t n, uint32_t x);

// The kernels in use, set by simd_kernels_init() (called from host_cpu_init()). They start at the baseline versions,
// so a call before that still works.
extern void (*sk_rgb24_rgba32)(const uint8_t *src, uint8_t *dst, size_t pixels);
extern int (*sk_any_gt_u32)(const uint32_t *v, size_t n, uint32_t x);

void simd_kernels_init(bool avx2);
