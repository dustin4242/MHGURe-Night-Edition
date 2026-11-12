# Running without AVX2, and using it when it is there

The port used to need AVX2: the recompiled game code was built with `-march=x86-64-v3`, so anything older than a
Haswell (2013) or an AMD Excavator (2015) faulted on the first instruction of the game code. It now builds for
x86-64-v2 (SSE4.2, 2009 and newer) and picks AVX2 code at startup on machines that have it.

## What runs at which instruction set

| Part | Instruction set | Why |
| --- | --- | --- |
| the executable (`runtime/`) | x86-64-v2 | `make`'s default flags: `-O2` with no `-march`, i.e. the x86-64 baseline. Always was. |
| the recompiled game code (`mhgugame.so` / `.dll`) | x86-64-v2 | `GENARCH` in the Makefile, was `-march=x86-64-v3` |
| `runtime/avx2_kernels.c` | AVX2 | compiled with `-mavx2`; only called through the pointers below |
| everything else (FFmpeg, FSR, ImGui) | x86-64-v2 | no `-march` on those rules |

`GENARCH=-march=x86-64` in the make line goes one step further (SSE2, 2003 and newer) for very old machines or
some virtual machines; `make gen` is not needed again, the game code is just recompiled.

## How a kernel is picked

`runtime/host_cpu.c` asks the CPU once, at startup, before the game code is loaded:

1. CPUID leaf 1: `OSXSAVE` (bit 27) and `AVX` (bit 28) in ECX, and `SSE4.2` (bit 20);
2. `XGETBV(0)`: bits 1 and 2 set means the OS saves the XMM and YMM state. Without that the CPU may have AVX2 and
   still fault: the OS is what decides whether `ymm0`-`ymm15` survive a context switch;
3. CPUID leaf 7, EBX bit 5: AVX2 itself.

All three yes: `simd_kernels_init(true)` points the kernel table in `runtime/simd_kernels.h` at
`runtime/avx2_kernels.c`. Any no: the same table keeps pointing at the plain C versions in
`runtime/simd_kernels.c`. The table starts at the baseline versions, so a call before `host_cpu_init()` is still
correct.

`MHGU_NO_AVX2=1` in the environment forces the baseline path, which is how the fallback gets tested on a machine
that has AVX2.

`log.txt` says which one is in use:

```
[cpu] sse4.2 yes, avx2 yes: avx2 kernels
[cpu] sse4.2 yes, avx2 no: baseline kernels
```

## The kernels

| Kernel | Used by | What it does |
| --- | --- | --- |
| `sk_any_gt_u32` | `vk_pages_changed()`, `runtime/nvn/vulkan/textures.c` | Any page of a texture newer than the generation the last check saw: 8 words per iteration, unsigned comparison by flipping the sign bit |
| `sk_rgb24_rgba32` | `upload_texture()`, `runtime/nvn/vulkan/textures.c` | RGB8 texture data to RGBA8 on upload: 8 texels per iteration, two `pshufb` lanes and the alpha bytes ORed in |

That is the whole list today. Everything else the port does per byte is already either done by the GPU or by
`memcpy`, which the C library dispatches to AVX2 itself.

How much of that a session actually executes depends on the game data and the platform, and for this dump the honest
answer is "little":

- **Textures.** A run of the village logs 543 textures, and their NVN formats are RGBA8 (0x25), the BCn block formats
  (0x42, 0x44, 0x49, 0x4b), depth/stencil and a few tiny RG32/RGBA32 ones. There is no ASTC (0x79-0x94) and no RGB8
  (0x19) in the dump, so `sk_rgb24_rgba32` has nothing to do here; the BCn data goes to the GPU as it is, because
  the drivers support it (the NVIDIA/Intel/AMD report in the log has `BC 1`; ASTC is `0`, and the game does not use
  it either, so the CPU ASTC decoder stays idle).
- **Page write tracking.** `vk_pages_changed()` only scans anything when the platform can report written pages,
  which is `GetWriteWatch` on Windows. On Linux there is no equivalent, `plat_written_pages()` returns -1, and the
  renderer hashes texture contents instead ("[vk] no page write tracking"), so `sk_any_gt_u32` is Windows-only in
  practice.

So on a Haswell running Windows the page scan is where the AVX2 path runs, every time a texture is checked; on Linux
the AVX2 kernels are present but a session of this game may never call one. Neither is where the frame time goes --
the recompiled game code is -- which is the next section.

## Why the game code stays baseline

The interesting question is whether the *recompiled* code should also be built for AVX2, since that is where the
port spends its time. Measured on the same 700 KB chunk of game code, three builds, no source changes:

| `GENARCH` | object size | AVX (VEX-encoded) instructions | 256-bit (`ymm`) registers | FMA |
| --- | --- | --- | --- | --- |
| `-march=x86-64` | 695232 | 0 | 0 | 0 |
| `-march=x86-64-v2` | 695232 | 0 | 0 | 0 |
| `-march=x86-64-v3` | 694544 | 1324 | 2 | 0 |

`-march=x86-64-v3` there mostly re-encodes scalar `movss`/`mulss`/`addss` in their VEX form: to the CPU it is the
same work, and to the file it is the same size. The reason is the shape of the code: `tools/lifter.py` emits scalar
C with loads and stores through `LD32`/`ST32`, and that is compiled at `-O2` with `-ffp-contract=off` (game code has
to round exactly as the ARM does), so there is next to nothing for the vectorizer to bite on.
Two `ymm` registers in a chunk of that size is noise.

So a second, AVX2 build of the game code would cost about 100 MB per release and twice the build time for a
difference that does not show up in a frame time: not worth carrying. The kernels above are the port's own data
loops, and they are where an AVX2 machine gets AVX2 code; how much of a session they cover is the table above, and
for this dump on Windows it is the page scan on every texture check.

If that ever changes -- a lifter that emits vectors, or a measured hotspot in the game code -- the same dispatch can
carry a second game library: `runtime/gamelib.c` already takes `MHGU_GAME_LIB` from the environment and refuses a
library whose id does not match, so a variant library would be a second file plus one line in host_cpu.c.

## Testing

```bash
make test-simd      # the AVX2 kernels against the baseline ones, random data, no game files needed
make -j8            # rebuild; a changed GENARCH throws the game-code objects away by itself (obj/gen.arch)
```

`make test-simd` compares the two implementations byte for byte over every size from 0 to 40 texels (the vector
loop's boundary and the scalar tail), 300 random sizes up to 5000 texels, and a 1024x1024 texture. It also checks
that the alpha byte is set and that neither kernel writes past its output buffer; the build is clean under
`-fsanitize=address,undefined`. On a machine without AVX2 it reports the detection result and exits 0, since there is
nothing to compare against there.

The game code can be checked for stray AVX from the outside:

```bash
objdump -d build/mhgugame.so | grep -c ymm     # 0
objdump -d build/mhgurecomp | grep -c ymm      # 0 on the executable as well
```

The second command counts the AVX2 kernel file's own code only if the link kept it; it is present but only ever
called through the table, so a `ymm` there is expected. The library must have none.

## Adding a kernel

1. Add the baseline version to `runtime/simd_kernels.c` and declare it, its AVX2 twin and a function pointer in
   `runtime/simd_kernels.h`.
2. Write the AVX2 version in `runtime/avx2_kernels.c` (already built with `-mavx2`; do not add `-mavx2` anywhere
   else, or the baseline promise breaks).
3. Point the pointer at one or the other in `simd_kernels_init()`.
4. Call through the pointer at the call site.
5. Add the pair to `tools/test_simd.c`.

Keep the AVX2 version a drop-in replacement: same arguments, same result, no state of its own, and no reads past
what the baseline version reads.
