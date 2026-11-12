# Performance audit — MHGURe Night Edition

**Date:** 2026-11-11 · **Method:** six parallel read-only static reviews (CPU core, Maxwell/NVN,
Vulkan execution, Vulkan resources/FSR, HLE/services/net, host/startup) plus a toolchain/build audit.
Nothing was modified, built or run; every frequency claim comes from the code and is marked where it
is an estimate. Numbers assume the documented workload (~1–3 k draws/frame, 50–200 passes, 60 fps)
and the R9 270X / 1080p figures in `docs/STATUS.md`.

**How to measure the wins below:** `MHGU_VK_STATS=1` (exec ms, passes, draws, copies, compiles),
`MHGU_CONTROL` `gpuprof` (per-pass GPU timestamps), `MHGU_FNPROF` (per-function counts/live),
`MHGU_CONTROL` `fntrace`/`peek`, and the built-in `stat_*` counters (`grep stat_ runtime`).
Findings marked *(Linux)* / *(Windows)* only apply there; the two platforms differ a lot right now
because Windows has page write tracking and Linux does not.

Legend: impact is expected end-to-end gain (H/M/L), effort and risk are implementation cost. All of
this is opt-in-improvement material; none of it is a required fix. §11 records what was actually built
and measured on 2026-11-11, which corrects several of the estimates above.

---

## TL;DR — the 14 changes I would make first

| # | Change | Where | Impact | Effort |
|---|---|---|---|---|
| 1 | Give the lifted functions a separate non-aliasing memory pointer so guest stores stop forcing `c->r[]` reloads (35 % of all host instructions are `Cpu`-struct traffic today) | `cpu.h:58-79`, `tools/gen.py:408` | **High** (est. 10–20 %, prototype first) | M |
| 2 | Try `-O2` for the 294 MB of lifted game code (currently `-O1`) | `Makefile:27` | **Low** (measured +1.3 %, §11 — not the 10–30 % the estimate assumed) | L (done) |
| 3 | Reuse descriptor sets per draw (hash the bound buffers/views) instead of allocating + writing 2 sets every draw | `exec.c:3458,3949-3951` | **Negative** (tried: +2 % CPU in the town, §11) | M (done, reverted) |
| 4 | Build the driver/rscale/zero constant blocks once, not per draw | `exec.c:3147,3156,3483-3497` | **Low** (landed; below the ±0.05 ms noise at 4K town) | L (done) |
| 5 | Route index buffers through the existing per-submission copy cache, and drop the second max-index scan | `exec.c:3982-3999` | **Low** (landed; below the noise floor in the town) | L (done) |
| 6 | Remove the two redundant ALL→ALL barriers (begin_pass, ms_resolve) and gate the rest behind a "something was recorded" flag | `exec.c:1580,1329,189` | **High (GPU)** | M |
| 7 | Only recreate the swapchain on `OUT_OF_DATE`, never on a bare `SUBOPTIMAL` (currently can idle the device every frame) | `device.c:611-613,488` | **High** on affected drivers | L |
| 8 | Skip the full-image zero clear when the upload is about to overwrite every texel | `textures.c:329-341` | **High** at area loads | M |
| 9 | One control word for `FN_ENTER` (currently 2 GOT loads + 2 branches per guest call) | `cpu.h:88-91` | **High** on call-heavy code | L |
| 10 | Pack NZCV into one word instead of four scattered flag fields | `cpu.h:68-71,142-159` | **High** on integer code | M |
| 11 *(Linux)* | Give `plat_written_pages()` a real implementation (userfaultfd-wp / mprotect+SIGSEGV) — without it texture hashing and the vertex cache fall back to CPU copies | `platform/linux.c:138` | **High (Linux)** | H |
| 12 | Fix the dispatch cache hash (it aliases every 16 KB of code) and stop paying 64 KB of TLS per thread | `dispatch.c:320-358` | M–H | L |
| 13 | Expand quads/fans straight into the transient ring instead of malloc/free per HUD draw | `exec.c:3586-3602,3969` | M–H (UI) | L |
| 14 | Buffer stderr/log (it is `_IONBF`) and gate `LOG` behind a verbosity check | `main.c:239`, `platform/linux.c:234`, `rt.h:10` | M (startup + any hot log) | L |

Items 1–2 are the CPU-side code-generation wins; 3–5 and 13 are the per-draw cluster and compound
(fewer bytes per draw also means a smaller transient ring, fewer chunk switches and less PCIe
traffic). Items 1 and 3 are the two "single biggest lever" candidates and should be prototyped
before committing to either.

Outside this table, the three cheapest startup fixes in §6.1 (hash-based symbol resolution, a cached
RomFS index, and moving arcmerge's cached early-return above the archive reads) are probably the
largest launch-time wins available, and §5.4/§5.6 (adler32, inflate copies, IPC zeroing) are the
cheapest load-time wins.

---

## 1. Per-draw CPU on the render thread

`draw()` is `exec.c:3764-4141`; the steady-state path records ~10 Vulkan calls per draw plus a large
amount of CPU-side bookkeeping.

1. **Two descriptor sets are allocated, re-written and re-bound on every draw** — `exec.c:3949-3951`
   (`stage_set()`), `exec.c:275-294` (`vk_alloc_set`). `vkAllocateDescriptorSets` + one
   `vkUpdateDescriptorSets` with 6–20 writes, twice per draw, with no reuse even when two consecutive
   draws bind the same textures/UBOs. At 2 k draws that is ~4 k allocations + 4 k updates + 2 k
   binds per frame, the largest single block of driver traffic in the file. Fix: direct-mapped cache
   keyed on (layout, buffer/view/sampler tuples) per submission slot; the sets already have
   per-submission lifetime because the pools are reset per slot.
2. **Constant blocks are rebuilt per draw, per stage** (`cbuf_info`, `exec.c:3154-3178`): cbuf 0 is a
   2 KB zeroed stack array + 16–32 stores + a `vk_transient_copy` into write-combined memory;
   `rscale` (`exec.c:3483-3488`) is a 1072-byte stack fill + copy that is *constant*; `zero_buffer()`
   (`exec.c:3147-3152`) copies 0x200/0x1000 zero bytes per binding. A GMem shader alone burns 4 KB
   of ring + 4 KB memcpy per draw. Fix: persistent buffers for the constant tables, and a
   per-frame zero block sliced for the rest; for cbuf 0, cache on the sbuf table generation.
3. **Index data is copied per draw with no dedup** — `exec.c:3982-3999` uses
   `vk_transient_copy(src, count*isz+4)` and then *also* scans the indices for `max_index`. Vertex
   data (`vcache`/`guest_copy_vertex`) and UBOs (`guest_copy`) already dedupe per submission and are
   invalidated by `gcopy_gen`; indices just don't use them. A 1 M-index frame is 2–4 MB memcpy
   *per pass that draws it*. Fix: `guest_copy(iaddr, size)` (or `vcache()`), and fold max-index into
   a cached per-range value.
4. **`PipeKey` (672 bytes) is zeroed, filled and FNV-hashed every draw** — `exec.c:3812-3815`,
   `exec.c:2677-2691`. 672-byte memset + ~1 100 cycles of hashing, before the pipeline can bind.
   Fix: hash only the ~10 varying fields, or keep a `memcmp` fast path for runs of identical state
   (see also 3.4/3.6 about the table's hash-only keying).
5. **Each sampled texture slot is resolved three times per draw** — `ms_resolve_sampled(0/1)` at
   `exec.c:3795-3796`, again in `resolve_feedback()` (`exec.c:3066-3139`), again in
   `stage_set → texture_info` (`exec.c:3433-3453`); each resolution does index math, a pool
   indirection and bounds checks. Fix: resolve once per draw into small arrays; precompute the slot
   index at reflection time.
6. **A `malloc`/`free` pair per non-native draw (all HUD quads/fans)** —
   `expand_indices()` `exec.c:3586-3602`, used at `3969-3999`. Hundreds to >1 k heap round-trips per
   menu frame on the render thread. Fix: expand directly into the ring (`vk_transient` + write loop).
7. **Depth-only prepass draws still allocate and bind an empty second descriptor set** —
   `make_layout` always builds two sets (`exec.c:2158-2166`), `draw()` binds both
   (`exec.c:3949-3951`). Roughly halves the descriptor work on prepass draws. Fix: leave
   `set_layout[1]` null when it has 0 bindings, bind 1 or 2 sets.
8. **`stage_set()` puts ~12 KB of arrays on the stack per call (~24 KB/draw)** —
   `exec.c:3459-3461`: `VkWriteDescriptorSet w[96]`, `VkDescriptorBufferInfo bi[96]`,
   `VkDescriptorImageInfo ii[96]`. Fix: size by actual binding count or a thread-local scratch.
9. **Non-16:9 setups pay the HUD re-anchor path per draw** — `ui_preview_map()` `exec.c:1757-1849`
   and the matrix-patch UBO copy at `exec.c:3195-3424` (up to 64 KB copied per HUD draw on
   16:10/ultrawide). Zero cost on 16:9. Fix: detect "this pass contains a model preview" once per
   pass; copy only `cbuf_size[3]` bytes.
10. **Small but free**: `vk_collect_dead()` runs a `vkGetSemaphoreCounterValue` per submission even
    with nothing dead (`exec.c:239`) — early-return on `ndead == 0`; `getenv("MHGU_DUMP_VERBOSE")`
    per `CMD_SET_DEPTH_CLAMP` (`exec.c:4478`); `config_live_*` atomics read 4–8× per draw
    (`exec.c:3716`); the movie-texture scan per draw (`exec.c:3732-3745`); two unconditional
    `plat_now_ns()` around every texture bind (`textures.c:618-626`, only used for the stats line);
    the FSR draw struct + texture scan when only RenderScale is active (`exec.c:3782-3787`).

---

## 2. GPU-side work and synchronization

1. **43 `vk_full_barrier()` call sites; the barrier is `STAGES_ALL → STAGES_ALL`, `ALL_WRITES → ALL`**
   (`exec.c:189-192`). A frame with 100–200 passes emits 150–250 of them. Two are provably redundant:
   the one at the top of `begin_pass` (`exec.c:1580`) is covered by the render pass's own external
   dependency (`exec.c:783-787`), and the leading one in `vk_ms_resolve` (`exec.c:1329`) is followed
   two lines later by an image barrier with the same masks. Fix: delete those two; emit the full
   barrier from `begin_pass` only when a transfer/compute command was recorded since the last one
   (one bool). Keep the wide masks for the documented GCN1 colour-cache workaround (`vk.h:89-104`),
   and A/B on the R9.
2. **`VK_SUBOPTIMAL_KHR` from present triggers `vkDeviceWaitIdle()` + full swapchain rebuild**
   (`device.c:611-613`, `488-492`). Some compositors/drivers return SUBOPTIMAL continuously
   (Wayland scaling, Mesa/XWayland, a 1-pixel extent mismatch), turning this into a per-frame device
   idle. Fix: only `OUT_OF_DATE`, an actual extent change (`device.c:531-532`), or N consecutive
   SUBOPTIMALs; drop the device idle and use the existing `vk_wait_seq` + `oldSwapchain` handover.
3. **Every new `VkImage` is zero-cleared over all mips/layers on the GPU, then the upload overwrites
   it** — `textures.c:329-341` vs the level/layer upload at `textures.c:569-613`; also one extra
   global barrier per created image. Fix: a `needs_zero` flag decided at creation; keep the clear for
   depth/stencil, MSAA/render-target-only and CPU-decoded BC/ASTC images.
4. **Feedback snapshots copy the whole image** (`exec.c:3123-3134`) whenever a draw samples a target
   written since the last snapshot, and each subsequent write invalidates it again — a ping-pong post
   chain can pay a full-resolution copy + barrier per draw. Fix: copy only the union of the draws'
   scissors (already computed for FXAA tracking), full copy as fallback.
5. **`scale_to_stand` wraps one full-screen blit in two global barriers** (`fsr.c:1493-1505`), once
   per frame with RenderScale active; narrow image barriers suffice.
6. **The output-size depth stand-in is cleared every frame** (`fsr.c:1701-1707`) even though the game
   clears its own targets — clear lazily when `leading_clears()` says the pass won't.
7. **Mid-frame readback submits and waits for the whole GPU** (`exec.c:4245-4246`): `renderer_submit()`
   + `vk_wait_seq()` inside the command list (quest-clear screenshot path; reachable per frame in
   principle). Fix: defer to the next submission boundary, or hand the wait to a worker.
8. **Render passes use `LOAD_OP_LOAD` + `vkCmdClearAttachments`** (`exec.c:767-777`, `2104-2116`)
   although `leading_clears()` already knows which attachments are fully cleared — a "cleared"
   render-pass variant would let a tiled driver skip fetching tiles it is about to clear.
9. **Masked-clear pipelines are created synchronously on the render thread** (`exec.c:2031-2121`),
   keyed by a 64-bit hash with no full compare and a 256-slot table that overwrites entries (leaking
   the old `VkPipeline` and mis-rendering on a collision). Move to the existing pipeline worker +
   full-key compare + destroy on eviction.
10. **MSAA resolve path**: lazy and shared (good), but each resolve records two ALL↔ALL barriers, an
   image copy and a freshly allocated/written descriptor set (`exec.c:1322-1386`) — fixed almost for
   free once §1.1 and §2.1 land.
11. **The `VkPipelineCache` is only serialised at exit** (`device.c:350-364`, `741`), and the exit is
    `_exit()`-based — any kill/crash loses every pipeline compiled this session, so the next run
    recompiles (first-use stutter). Save every N seconds / after a batch, tmp+rename as it already does.
12. **`pipecache_store()` does `fwrite` + `fflush` per new pipeline on the render thread**
    (`exec.c:2755-2769`): >100 pipelines at an area load = 100+ synchronous flushes exactly during
    transitions. Drop the flush (the loader already truncates a partial tail).

---

## 3. Caches, change detection and object lifetime

1. *(Linux/Android)* **`plat_written_pages()` is a stub returning -1** (`platform/linux.c:136-141`),
   so `vk_pages_changed()` always reports "unknown". Consequences:
   `upload_texture()` falls back to content hashing (`textures.c:522-558`): dynamic textures hash the
   whole texture every frame, static ones get a 4096-point scattered sample every 8th frame; and the
   device-local vertex cache (`vcache()`, `exec.c:429-509`) is effectively dead, so up to 4 MB vertex
   ranges are re-copied per submission. Fix: implement write watching (userfaultfd write-protect on
   the guest window, or `mprotect(PROT_READ)` + SIGSEGV handler that records the page and re-arms),
   or at minimum charge full hashes against the 4 MB/frame budget and make the vertex cache trust
   ranges for one frame.
2. *(Windows)* The converse: `GetWriteWatch` is a syscall per distinct sampled texture per frame
   (`textures.c:522-545` → `platform/windows.c:65-75`). One `GetWriteWatch` over the committed window
   per frame, feeding the existing page-generation table, removes hundreds of syscalls/frame.
3. **`views_forget()` scans all 16 384 view-table slots per destroyed/widened image**
   (`textures.c:685-688`) and `vk_framebuffers_forget_view()` scans 1024 slots × attachments per
   destroyed view (`exec.c:825-832`) — millions of iterations in one frame at a level transition.
   Track the slots each backend/view inserted.
4. **The render-pass cache silently stops caching after 256 entries and never frees**
   (`exec.c:749-799`); each overflow also changes `PipeKey.rp`, so *every draw* misses the pipeline
   table and builds a new pipeline. Make it a hash table with eviction (also frees the old pass).
5. **`pipe_slot()` keys pipelines by a 64-bit hash only, no full-key compare**
   (`exec.c:2677-2691`) — a collision silently renders with another pipeline's state; also blocks
   shortening the key (§1.4). Store a second hash word and compare both.
6. **`compute_program()` linear-scans 256 entries, has no disk cache, and past 256 compute shaders
   re-translates + re-creates the pipeline per dispatch** (`exec.c:3536-3548`); `prog_tab`'s probe has
   no bound (infinite loop if the 16 384-entry table fills, `exec.c:2607`).
7. **`tex_untrack()` is O(live textures) under a global mutex per texture finalize**
   (`objects.c:29-39`), and `nvn_textures_visit` holds the same lock while copying whole textures.
8. **`plist_drain()` is O(queued × list) (≈256 × 1 520 records) on the render thread during boot**
   (`exec.c:2892-2930`) — bucket the immutable shipped list by hash once at load.
9. **`format_supports()` does 2–3 `vkGetPhysicalDeviceFormatProperties` per texture creation,
   uncached** (`textures.c:280,303`); `tex_backend()` calls `vk_collect_dead()` per new texture
   (`textures.c:408-415`); the view table evicts on the 16th probe rather than LRU
   (`textures.c:690-713`); `getenv` per sampler creation (`textures.c:862-887`).
10. **Shader translation**: `sp()` calls `vsnprintf(NULL, 0, …)` and then `vsnprintf()` for *every*
    emitted fragment (`translate.c:40-53`), and `glsl_rewrite()` emits one `sp()` per token and per
    whitespace/punctuation char plus up to ~20 `strcmp` per identifier (`translate.c:1675-1757`).
    Translation is off the render thread and cached (in memory + `cache/programs.bin` +
    `cache/spirv/`), so this is cold-start/prewarm time (~2.3 ms × 6 311 programs), not frame time.
11. **`tegra.c:106-118` `copy_surface()` copies 16 bytes per iteration** for every uploaded surface —
    4–8× slower than a 64-byte/GOB-wise kernel; also `cmdbuf.c:23-29` memsets 80 bytes then
    overwrites 72 per recorded command; `vk_reflect()` allocates 5 times per shader compile
    (`exec.c:216-219`); `src_hash()` hashes ~27 KB byte-at-a-time per compile (`shaders.c:24-28`).
12. **Transient ring**: chunks are found by a linear scan and never shrink (`exec.c:320-341`);
    256-byte alignment per UBO copy wastes up to 255 B per descriptor (`exec.c:3142`). Mostly a
    consequence of §1.2/§1.3; align to `limits.minUniformBufferOffsetAlignment`.

---

## 4. CPU core, interpreter and lifted code

The measurable scale of the target (all counted in the existing tree): **100,756 lifted functions** (72,695 in `generated/main`, 28,061 in `generated/sdk`),
5.78 M source lines, **1.62 M guest memory accesses** (32 % of instructions), **344 k guest call
sites** (≈1 per 14.5 instructions), 392 k conditional sites, 60 k indirect branches, and **4.6 host
instructions / 16.3 bytes per lifted instruction** (`main_000.o`). 35 % of those host instructions
are reads or writes of the `Cpu` struct.

### 4.1 Guest registers live in memory, and guest stores block reload elimination — 35 % of host instructions

`cpu.h:58-63,74`: guest memory is accessed through `may_alias` pointer types
(`u32u`, `f32u`, …), and every lifted function takes `Cpu *restrict c` (`tools/gen.py:408`) while
stores go through `G2H(a)` pointers that the compiler cannot prove disjoint from `*c` (and
`-fno-strict-aliasing` removes the rest of the type-based escape). Result: every store in a lifted
function forces `c->r[]` reloads, e.g. `f_200e14` reloads `c->r[13]` after each of a sequence of
stores (`obj/gen/main/main_000.o`). That is on the path of 32 % of instructions.

Fix (needs a 10-minute prototype first): pass a second, genuinely non-aliasing memory pointer
(`void f_x(Cpu *restrict c, uint8_t *restrict mem)`, `LD32(a)` → `*(u32u *)(mem + (uint32_t)a)`).
Two `restrict` parameters are guaranteed not to alias (C11 6.7.3.1) independently of TBAA /
`may_alias`, but whether clang actually uses that with `-fno-strict-aliasing` must be measured:
compile one file both ways and count `(%rbx)` reloads / host instructions. If it works it is the
largest single CPU win available; if it does not, table the idea.

### 4.2 The game code is at `-O1`

See §7.1 — this is the same lever from the toolchain side; typically 10–30 % on scalar
load/store-heavy code. Try `-O2` (and `-O2 -flto` for the gamelib as a second data point).
`-ffp-contract=off` must stay; nothing else in `GENFLAGS` requires `-O1`.

### 4.3 `FN_ENTER` costs two GOT-indirect loads and two branches per guest call

`cpu.h:88-91` is emitted at all ~100.8 k function entries: `if (g_fnprof) …` (a `volatile int`, so a
forced load) and `if (g_fnhook[a>>5] …) && fnhook_enter(…)` — both defined in the executable, both
preemptible, so each is a GOT load + value load in the gamelib. Roughly 7 host instructions per call,
≈7 % of the dynamic stream (1 call / 14.5 instructions).
Fix: keep one control word in the `Cpu` (there are spare bytes at `cpu.h:31`) — e.g.
`if (__builtin_expect(*c->rt_check, 0)) rt_entry_slow(c, a);` — and move both the profiler and the
hook bitmap behind the slow call. The fast path becomes 2 instructions. Hooks keep working
(`rt_entry_slow` runs the same logic).

### 4.4 Flags are four separate fields

`cpu.h:68-71,142-159`: `f_subs`/`f_adds`/`f_sbcs` store `nf, zf, cf, vf` separately (~6–8 host
instructions, up to 4 stores), and every `COND_*` consumer (392 k sites) reloads them. Pack NZCV into
one `uint32_t` and redefine `COND_*` as shifts/masks (`apsr_get/set` become shifts too). One store
and one load instead of four and two. Medium risk: every flag producer/consumer, the interpreter and
`vfp_mrs_flags` must change together; `tools/test_lift.py --interp` covers it against Unicorn.

### 4.5 Predicated instructions become unpredictable branches

`tools/gen.py:484-486` emits `if (COND_EQ) { … }` for A32 conditional instructions (392 k sites).
For single-destination ALU ops this should be a value select (`c->r[0] = COND_EQ ? expr : c->r[0]`,
which becomes `cmov`) — keep the branch for stores/calls/returns. `-O2` may do some of this by
itself (§4.2).

### 4.6 The dispatch cache aliases every 16 KB and costs 64 KB of TLS

`dispatch.c:320-322,352-358`: `dcache[4096]` is indexed by `(pc >> 2) & 4095`, i.e. pc bits 2..13
only, so any two indirect targets 16 KB apart collide and re-run `dispatch_slow` (HLE range check +
linear scan of up to 1024 overrides + binary search over 72 695 entries) on every alternation.
Mix in the high bits (`((pc >> 2) ^ (pc >> 14)) & 4095`), make it 2-way, or make it a shared
(non-TLS) table — the reader already validates `addr`, so a lock-free shared table is safe and all
threads reuse the hot entries.

### 4.7 Interpreter (mods, cheats, Forge plugins) — 20–30 host instructions of overhead per instruction

The interpreter is only used for un-lifted code: mod subsdks (`loader.c:399-402`), Forge plugins
(`forge.c:1364,1432`), and any function a cheat patches (`cheats.c:279-289`). Its loop
(`interp.c:1574-1592`) does two TLS stores, two GOT-indirect hook-bit probes, watch/read-watch loads,
`interp_owns()`, and an out-of-line `interp_exec()` with a second switch level, plus `*link` through a
pointer. Fixes: one `g_interp_dbg` word read once per run; drop `g_ip_last` from the loop; return the
link flag in the high bit instead of a pointer; inline `interp_exec`; replace `cond_pass` with a
16-byte table; cache the hook bitmap base. For cheat users, narrowing the interpreted range to the
patched instruction window (instead of the whole containing function) removes the 30–100× cliff.

### 4.8 NEON is always scalarised per lane

`tools/neon.py` emits `c->v.s[i]` loads/stores per lane: a `vadd.i32 q0,q1,q2` becomes ~16 host
instructions instead of ~3. Add a whole-register 128-bit lowering for the 1:1 ops
(VADD/VSUB/VMUL/VMLA/VMLS/VMIN/VMAX/VCGT/VCGE/VCEQ/VTST/VAND/VORR/VEOR/VBIC/VBSL/VSHL(imm)/VMOVN/
VZIP/VUZP) as `static inline` SSE2/SSE4.1 helpers in `runtime/neon_wide.h`; `Cpu.v` is 16-byte
aligned and a q register starts at an even d, so aligned loads are legal. Keep the scalar path as the
reference for the hairy ops. ~70 k vector instructions statically; the gain depends on how hot NEON
is in a frame (measure with `MHGU_FNPROF`).

### 4.9 Generator micro-optimizations (low risk, each small)

- `pop {…,pc}` returns store `c->pc` and load the return address, both dead when the popped pc is the
  function's own entry `lr` (51 551 sites, `lifter.py:500-515`) — emit a bare `return` in that case.
- Jump tables (`gen.py:471-481`, 6 023 sites) load the guest table and compute `c->pc`, although the
  generator already resolved the targets at build time — emit `switch (c->r[idx]) { case 0: goto L; }`
  and drop the `rt_dispatch` fallback for impossible defaults.
- `movw`+`movt` pairs (26 030 sites) emit a dead MOVW store + a reload in MOVT — fold the pair into
  one constant store (`lifter.py:199-205`).
- `vcmp` + `vmrs APSR_nzcv` (≈22 k sites) is three compares + an FPSCR read-modify-write + four flag
  stores; compute the 4-bit field branchlessly and fuse the pair (`vfp.h:16-28`).
- `ldm/stm` emits N independent loads/stores; a `memcpy`-shaped pair copy for register pairs lets
  clang emit `movdqu` (`lifter.py:500-520`).
- `-fPIC` on the gamelib (needed) plus PLT for the few runtime helpers is unavoidable; the GOT loads
  in `FN_ENTER` disappear with §4.3, the interpreter ones with §7.6.

### 4.10 Checked and already good in the CPU core

No TLB/page-table/bounds/null checks or SMC checks in lifted code — guest memory is one flat 4 GB
window and `G2H` is a single add, with `GUEST_BASE` hoisted per function. Direct `bl` becomes a host
call, guest tail calls use `musttail` (no host stack growth), returns are host returns, literal-pool
loads become constants, jump tables are real C switches, HLE slots are O(1), `dispatch_lookup` is a
binary search, flags helpers are inlined, no malloc/locks/logging/getenv/atomics on the instruction
path, the exclusive monitor is CAS only for `strex`, and AVX2 kernel selection is a function pointer
set once at startup.

---

## 5. HLE, kernel, services and network

Most of this layer is opt-in (traces, Discord, network, movies). The always-on costs are the kernel
mutex emulation, the audio renderer, HLE libc, and the IPC/filesystem path.

### 5.1 Kernel / SVC

1. **Every uncontended guest mutex lock/unlock takes the one global kernel mutex and walks the wait
   list** — `kernel.c:306-327`, `mutex_release` at `kernel.c:267-284`. nn::os Mutex/CondVar are not
   HLE'd, so every guest `LockMutex` is an SVC pair: two `pthread_mutex_lock/unlock` on the
   process-wide `g_klock`, an atomic load, and a full `g_waitlist` scan on unlock. That serialises
   all guest threads (render/main/audio/net). Fix: a lock-free fast path
   (`__atomic_compare_exchange_n(word, &zero, tag)` for lock; store 0 when there is no `WAITERS_BIT`
   for unlock), and early-out `mutex_release` when there are no waiters. **Impact: high** (the most
   frequent SVC in gameplay). **Risk: medium** (concurrency).
2. **`kernel_signal()` wakes every thread blocked in `WaitSynchronization`, not just the waiters on
   that object** — `kernel.c:70-75`, called from vsync and the audio event every frame. N blocked
   threads means N futex wakes and N handle-array rescans per signal. Fix: per-object waiter list, or
   record the object each thread waits on. **Impact: medium-high** (2 signals/frame × threads).
3. **`MapMemory`/`UnmapMemory` copy the whole range** and `plat_commit` walks it in 64 KB chunks
   (`kernel.c:575-585`) — O(size) instead of a page-table remap. Whether this is hot depends on
   whether the game/NVN glue maps per frame; `MHGU_TRACE_SVC=1` answers it. If hot: mapping table +
   copy-on-fault. **Impact: unknown, potentially high; effort high.**
4. Low: `handle_alloc` linear-scans 4096 entries per new handle (`kernel.c:36-45`); guest threads get
   256 MB host stacks (`kernel.c:221-228`, virtual only but 5 GB of address space with ~20 threads).

### 5.2 Audio (always on while the game plays sound)

1. **The renderer clears all 24 mix buffers of every mix every period** — `services/audio.c:951-952`:
   `memset(r->mix[m].buf, 0, sizeof(float) * MIXBUF_MAX * N)` although only `m->bufs` (typically
   1–6) are used. At 48 kHz/480-frame periods with ~5 mixes that is ~230 KB of memset per period
   (~23 MB/s, 1–2 % of a core). One-line fix: `* r->mix[m].bufs * N`. **Impact: medium-high, risk none.**
2. **Audio mixing is fully scalar, and the gain is ramped per sample even when it is constant** —
   `audio.c:966-1000` (Hermite, already fast-pathed for `ratio == 1 && f == 0`) and
   `audio.c:1030-1036`: `o[i] += smp[i] * (g0 + dg * i)` for every voice channel every period. When
   `g0 == g1` (steady volume, the normal case) a constant-gain loop would vectorise to FMA and be
   ~4× faster; keep the ramp only on frames where the volume actually changed (the code already
   tracks `prev_volume`). **Impact: medium-high** (continuous, audio thread).
3. The legacy `audout` device polls with 1 ms sleeps while idle/draining
   (`audio.c:1724-1728`) — only opened for movies; use a condvar with a 10–20 ms cap. Low.

### 5.3 HLE calls

- **`nn::memcpy/memmove/strlen/strcmp…` are HLE host calls** (`hle/libc.c:65-72`): each goes through
  `rt_dispatch` → HLE slot → libc, and `memcpy` additionally calls `memcpy_watch` (a load + branch)
  on every copy. Thousands of small copies per frame. Measure with
  `MHGU_HLE_SKIP=memcpy` + `MHGU_FNPROF=1` before changing; cheap wins regardless: drop
  `memcpy_watch` from the hot aliases, use `memcpy` (not `memmove`) in the `memcpy` aliases.
  **Impact: medium-high but unproven.**
- **`fnhook_call_original()` does a linear `interpreted()` scan plus a binary search per hooked
  call** (`dispatch.c:260-271`); hooks are set only at init, so store the original `GuestFn` in the
  hook entry. Affects `weaponfusion:lookup` and `inflate:inflate_fast/adler32` (thousands of calls
  per archive load). **Impact: medium at loads/equipment-box scroll.**
- **`stub_handler` logs every unimplemented IPC command to unbuffered stderr** (`ipc.c:16-21`). If
  any unimplemented command lands on a per-frame path this alone can dominate a frame; rate-limit it
  per (service, command) as `pctl_service` already does (`misc.c:82-87`). **Risk of a nasty surprise,
  zero cost to fix.**

### 5.4 Loading and I/O

- **`adler32` is a scalar byte loop** (`hle/inflate.c:212-227`); the file's own header records it as
  the next 15 % of loader-thread time after `inflate_fast` moved to host. An SSE2/AVX2 adler32
  (psadbw/madd) or zlib's `adler32_z` is 4–8× faster. **Impact: medium-high on load screens.**
- **`inflate_fast` copies long matches 3 bytes at a time** (`inflate.c:156-178`). When
  `dist >= len && len >= 16`, a `memcpy` is safe and much faster; keep the byte loop for overlaps.
- **`services/fs.c` builds two 2 KB path buffers and a 1200-byte tmp path and runs 2–4 `snprintf`
  per IPC call** (`fs.c:323-331`, `140-152`), even though `tmp_path` is only used by 3 commands, and
  `pending()` does a 16-entry `strcmp` scan. Skip the tmp path unless needed; reuse a thread-local
  host-path buffer.
- **RomFS reads take a global fd-cache mutex and scan 64 entries per read** (`romfs.c:285-304`) —
  single reader thread in practice, so it is pure overhead while streaming; keep the fd in the `File`
  and use one fd per open. `romfs_host_path()` is an O(all files) `strcasecmp` scan
  (`romfs.c:322-327`, movies/GMD only) — `bsearch` on the already-sorted table.
- **IPC zeroes ~1.3 KB of stack structs per request** (`ipc.c:90-91`: `IpcReq`/`IpcResp` `{0}` plus a
  256-byte message memset), and `service_new()` does `calloc` + `strdup` per returned object with no
  free path (`ipc.c:42-52`). This is paid on every `IFile`/audio/vibration request;
  `SendVibrationValues` alone is a full IPC round trip **200×/s** (`services/misc.c:208-221`).
  Initialise only the used fields, use a static-name constructor, free on release.

### 5.5 Hooks that run per frame

- **`pausepages` rebuilds each row's label/value with memset + `strlen` + byte-at-a-time stores +
  `snprintf` on every layout pass** (`hle/pausepages.c:621-688`, `put()` at `226-230`,
  `rr_text`/`fov_text` at `291-296`, `255-260`). Pause menu only, but it is the most wasteful
  always-redone work there; cache per row with a dirty flag set when the setting changes.
- **`itembar` takes two mutexes and a clock reading on every player update** (`hle/itembar.c:54-74`,
  installed unconditionally); it only needs an `__atomic_exchange_n` of the step counter.
- **`hatestep` scans up to 32 slots per monster attention update** (`hle/hatestep.c:23-46`); index by
  a hash of the monster pointer or cache the slot.
- **`presence_tick` builds the whole Discord payload (≈10 `snprintf`s) once a second even when Discord
  is off** (`hle/presence.c:152-176`); one early-out. Same for the party SHA-256 while offline.
- `camera_jitter` (FSR only) locks + memsets a 128-byte record per projection (`hle/camera_jitter.c:
  149-185`) — drop the redundant memset; `textinput_poll` copies the whole 2 KB buffer under a mutex
  every frame while a text entry is open (`textinput.c:64-72`) — return early when unchanged.

### 5.6 Network and movies (opt-in)

- **`ldn::Scan` blocks the game thread for up to 400 ms and creates/joins a thread per call**
  (`net/ldn.c:1495-1521`), plus up to 1.5 s per join address. Run discovery on the persistent ldn
  worker and return the last cached result. **Impact: medium, Local Play only.**
- **Movie playback interleaves U/V planar→NV12 with a scalar byte loop per frame** and re-switches
  on `f->format` inside the sample loop (`hle/movie.c:586-640`); use `sws_scale` with an NV12 context
  (SIMD) or a SSE2 pair-copy, and hoist the format switch. Decoding itself is driven by the game, so
  there is no over-decode or readback.
- `vsock`/`ldn` are inactive offline (no thread, socket or per-frame work).

### 5.7 Checked and already good in the HLE layer

HLE addresses index `hle_slots[]` in O(1); the dispatch cache fronts a binary search; SVC dispatch is
one switch; `ENV()` caches per call site; live config values are SDL atomics; `emtrace` and the other
tracers install hooks only under their env vars; `inflate_fast` already moved to host; RomFS data uses
`pread` with a fd cache and a binary search per chunk (no open per read); `fsync` only on `IFlush`/
commit; `vsock` uses a condvar (no polling); `ldn` is state-gated; presence's GMDs are cached per
quest; `vi.c` sleeps on an absolute, drift-corrected deadline and only writes the frame-rate field
when it changes; `pctl` already rate-limits its logging.

**Not reached in this pass:** `forge.c`/`forge_imgui.c` internals (its `frame_update` hook and 49
HLE_FUNCs), `services/am.c` end-to-end, ldn relay/Discord protocol internals, and the movie demuxer.

---

## 6. Host frontend, startup and configuration

### 6.1 Startup — the three cheapest big wins are here

1. **Symbol resolution is O(imports × symbols) with `strcmp`, twice over.** `loader.c:191-200` walks
   every symbol of every module (main alone has tens of thousands) for every relocation; each lookup
   first runs `hle_lookup_impl()`'s linear `strcmp` scan over ~231 entries (`dispatch.c:37-44`), and
   `hle_register()` linear-scans `hle_slots` per new name. Thousands of imports × tens of thousands of
   symbols ≈ 10^7–10^8 `strcmp` per start. Fix: build a name→address hash over all modules' symtabs
   once at first relocate (and a hash set for the HLE names); lookups become 2–3 probes. **Impact:
   likely the largest non-I/O startup item.** Risk low, effort medium.
2. **The RomFS index is rebuilt by a full recursive `opendir`+`stat` of ~35,531 files on every
   start** (`romfs.c:54-82`, called from `romfs_open` at `romfs.c:190-207`), with a `strdup` per
   path and nothing cached between runs. Serialize `{rpath, host path, size, mtime, offset}` to
   `cache/romfs.idx`, keyed by a cheap directory stamp (root mtime + count); rebuild the C tables
   from the file on a hit. **Impact: high** (tens–hundreds of ms). Risk medium (stale index).
3. **A cached arc merge still reads all three archives in full.** `arcmerge.c:105-118` opens `orig`,
   `prev` and `mod` (each `arc_open(..., false)` mallocs and reads the whole archive) and builds the
   merged entry list *before* the `if (cached) { ok = true; goto done; }` at line 138 — and for
   `orig != prev` (two mods replacing the same `.arc`) that is tens of MB re-read every start just to
   log overlaps. Move the cached early-return above `arc_open`; keep the overlap report behind a
   debug env. **Impact: high, effort: one move.**
4. `merge()` is O(mod files × target files) with `strcasecmp` (`romfs.c:86-119`); a 5 k-file texture
   mod into a 5 k-file tree is ~2.5×10^7 comparisons per mod. A per-directory hash (lowercased name →
   index) makes it linear. **Impact: medium-high with large mods.**
5. Mods are re-enumerated per module: `mods_patch_module()` runs once per module (12×) and
   `mods_find_dir()` restarts a breadth-first walk from the root once per level (`mods.c:316-343`,
   `modorder.c:31-61`), so depth 3 ≈ four walks of the same tree each time. Memoize the `exefs/`
   paths for the whole boot. **Impact: medium with mods.**
6. Every candidate subsdk is fully read and LZ4-decompressed just to test the first 8 bytes of its
   name (`loader.c:118-143`, called up to 10× per mod). Read the header/first block or cache the
   verdict per (path, size, mtime). **Impact: medium with Forge mods.**
7. The RomFS sanity check walks the whole tree a second time (`main.c:194-199`,
   `platform/linux.c:277-296` — `plat_count_files` + `spelled_on_disk`) right before the index scan.
   Use the index cache's count, or check only the three sentinel files the error messages care about.
8. **Logging is unbuffered** — see §7.4; from the startup side it is one `write()` per line for the
   hundreds of boot lines.
9. The INI parse is two passes and `strtod` per numeric row (`config.c:959-990`), but it is only a few
   hundred lines; the one actionable bit is `append_missing_sections` re-scanning `DEFAULT_INI` and
   possibly appending to the ini on a normal start. Low.

### 6.2 The host thread

10. **The host loop is a 4 ms `SDL_WaitEventTimeout` poll that rebuilds the entire input state on
    every wake** (`ui.c:293-352`): with no events it runs 250×/s, with a 1 kHz mouse it runs ~1000×/s,
    and each wake runs `host_input_update()` (~28 bind queries, gamepad reads, two texture ticks,
    rumble, control poll, textinput) plus `armor_class_frame()`. Nothing is gated on the game actually
    presenting a frame. Fix: rebuild state only when `g_present_count` changed (keep the event drain
    for input edges), and raise the idle timeout to ~8 ms. **Impact: medium-high** on the thread that
    shares L3 with the guest.
11. **`bind_held()` re-reads atomics and rescans slots for every one of ~28 queries** and calls
    `plat_now_ns()` each time (`host_input.c:61-71,747-759`) — ~60 atomics and ~60 clock reads per
    wake (15 k–60 k/s). Keep a plain `bind_snapshot[][]` + generation counter, rebuilt only when the
    config changes, and hoist one `now` per pass. **Impact: medium** (hottest host function).
12. **`icons_tick()`/`hd_ui_tick()` are called from `input_set`** (`hle/input.c:39-44`), so they pay
    two clock readings per wake, and once a second each walks **every NVN texture** and FNV-hashes any
    whose size matches a known glyph/HD-UI size (`ui_icons.c:168-177`, `ui_hd.c:270-279`), holding the
    texture-registry mutex the render thread uses. Run them from a 1 Hz timer/present path and only
    hash after `icons_set`/`hd_ui_set` invalidates. **Impact: medium** (periodic render-thread stall).
13. `armor_class_frame()` reads the player struct (5 slots × 11 words) on every wake although
    `cheats_frame()` is frame-gated (`ui.c:351`, `hle/armor_class.c:169-211`) — add the same
    `g_present_count` guard. Low-medium.
14. The gamepad is polled function-by-function (~24 SDL calls per wake: 10 buttons + 4 face + 2
    labels + 2 triggers + 4 axes, `host_input.c:762-810`); one `SDL_GetGamepadState()` snapshot
    replaces them. Low-medium.
15. Forge's per-frame input hook calls `SDL_GetKeyboardState()` (SDL's keyboard lock) and copies
    256 bytes twice per frame (`forge.c:949-958`) although `host_input.c` already tracks `keys[]` from
    events — expose that array instead. Low-medium.
16. **`config_editable()` returns the whole `Config` (~6.9 KB: 5 144 B of `HpScale`, 1 376 B of
    strings) by value**, and `overlay.cpp:432` calls it per frame while the chat line is open
    (`config_store.c:112`). Add `config_menu_theme()`/`config_mute_in_background()` atomics like the
    other live values. **Impact: medium-low.**
17. The overlay fast path takes three mutexes per frame even when nothing is drawn
    (`overlay.cpp:548-568`); the notice/chat flags can be atomics and `g_snap_lock` only needs taking
    when `g_snap.Valid` must be cleared.
18. Overlay draw lists are `CloneOutput()`-ed and freed per frame (`overlay.cpp:636-647`) — only while
    the overlay draws; pool or copy-on-count-change. Low-medium.
19. Forge plugin ImGui calls each do a full `ImGuiIO`/`ImGuiStyle`/`ImGuiPlatformIO` mirror
    push+pull (`forge_imgui.c:281-289,342-346`), so a 200-widget plugin menu does ~400 whole-object
    syncs per frame. Dirty-flag the mirrors. **Impact: medium with plugin UI.**
20. Small: a rumble-end `LOG` per event on unbuffered stderr (`host_input.c:719-730`);
    `config_live_chat_key()` called twice per key-down and the wheel path scans all bind slots
    (`host_input.c:592,73-82`) — fold into the bind snapshot; the scripted-input list is scanned on
    every wake (`host_input.c:836-843`) — only when it is non-empty and a frame advanced.

### 6.3 Platform

- **`plat_sleep_until_ns()` busy-spins the last 200 µs with `sched_yield()` on every sleep**
  (`platform/linux.c:169-181`). The vsync thread calls it every display period and every guest
  `SleepThread` goes through it, so at 60–240 Hz that is up to 200 µs of a spinning core per frame
  (1–5 % of a core) contending with the guest. Spin only the last 20–50 µs, return immediately when
  the deadline has passed, and use one `clock_nanosleep` to the (t − 50 µs) deadline. Low-medium.
- `plat_now_ns()` itself is a vDSO `clock_gettime` (~20–25 ns); the win is fewer call sites
  (§6.2 #11/#12), not a different clock.
- No per-frame `getenv` or file I/O in normal play: `control_poll` returns before any syscall when
  `MHGU_CONTROL` is unset, the pad/armor debug flags are latched once, `MHGU_FRAMERATE` is latched,
  the ini is parsed once, and config saves only happen from menu actions (with one `fsync`).

### 6.4 Checked and already good on the host side

`dlopen(RTLD_NOW)` once and the table entry is a single `dlsym`; live settings are `SDL_AtomicInt`
getters with no locks or string copies; the ini is parsed once and never re-read per frame; ImGui
context/fonts/backend are created once and `NewFrame`/`Render` are skipped entirely when nothing is
drawn; icon/HD-UI payloads are loaded on first use and memoized; `textinput_update` is one state
compare per wake; RomFS data goes through `pread` + a 64-entry fd cache and a binary search; arc
merges are content-keyed and cached on disk (the ordering is the bug, §6.1 #3); `cheats_frame` is
frame-gated and early-outs; settings icons/locale are compiled-in tables; keyboard edges are O(1) per
event; overlay input is queued and consumed on the game thread, so the host thread never blocks on
ImGui.

**Not reached:** `platform/windows.c`/`android.c` in full, `forge.c` beyond the frame hook and plugin
loader, `sandbox`/settings-panel sources outside `runtime/`, and the Discord/chat protocol internals.

---

## 7. Build and toolchain

1. **The game itself is compiled at `-O1`.** `GENFLAGS` (`Makefile:27`) covers all 383 files
   (`generated/main` 308 + `generated/sdk` 75, 294 MB of C) — the hottest code in the process. `-O1` has been there since the
   initial commit; the rationale in `docs/AVX2-FALLBACK.md` only justifies `-march=x86-64-v2` and
   `-ffp-contract=off`, not the optimisation level. Measured on `main_203.c`:

   | flags | wall time | object size |
   |---|---|---|
   | `-O1` | 2.56 s | 430 720 B |
   | `-O2` | 3.01 s | 452 016 B |
   | `-O3` | 3.10 s | 459 648 B |

   i.e. `-O2` costs ~18 % more compile time and 5 % more code for (potentially) much better
   scheduling/register allocation in the lifted scalar code. This is the single biggest never-tried
   knob; it needs the differential tests (`tools/test_lift.py`, `make test-*`) and a long play
   session to rule out latent UB in the generated C.
2. **The runtime is built for baseline x86-64 (SSE2 only).** `RTFLAGS` (`Makefile:177`) does not
   include `$(GENARCH)`, so `exec.c`/`textures.c`/`fsr.c` get no SSE4.1/4.2/POPCNT even though the
   project already requires x86-64-v2 (2009+ CPUs) for the game code. Add `-march=x86-64-v2` to
   `RTFLAGS` — free, and consistent with the stated floor.
3. **No LTO and no PGO anywhere.** ThinLTO for the runtime objects is cheap to try; full LTO over
   294 MB of generated C is not. PGO of the gamelib (instrument → run a captured session → rebuild)
   is the theoretically largest win but also the largest build/complexity cost.
4. **Logging is unbuffered and unconditional.** `LOG` is `fprintf(stderr, …)` (`rt.h:10`) and both the
   console and the redirected `log.txt` are set to `_IONBF` (`main.c:239`, `platform/linux.c:234`),
   so every log line is one `write()` syscall. 700 `LOG(` call sites; the ones that can run per frame
   or per draw are the expensive ones. Fix: 1 MiB block buffer with a flush on `FATAL`/exit (keep
   `_IONBF` only when stderr is a tty), and a global verbosity check so hot-path logs are one
   predictable branch.
5. **Stack protector is on (Ubuntu clang default) for runtime objects**: 46 canary sites in
   `obj/rt/nvn/vulkan/exec.o`, 7 in `interp.o`, 0 in generated code. `-fno-stack-protector` on the
   runtime removes a TLS canary load/store from every protected function — small, free, local-app
   trade-off.
6. **The executable exports 3 276 dynamic symbols** (`-rdynamic`) because the dlopen'd gamelib needs 61
   of them. Everything else is default-visibility, so the runtime's own globals are addressed through
   the GOT in this PIE. `-fvisibility=hidden` for the runtime plus an export list (linker version
   script / `--export-dynamic-symbol`) turns every `g_config`/`g_fsr_on`-style access into a direct
   RIP-relative load and shrinks `.dynsym`. Medium effort because a missed export is a load-time
   failure.
7. **Per-call hook/profiling prologue** — see §4.3. Static call-site counts in
   `build/mhgugame.so`: 106 121 `fnprof_hit@plt`, 106 118 `fnhook_enter@plt`, 72 571 `rt_dispatch@plt`
   (the direct `f_*` calls themselves are direct — 341 986 of them, zero PLT, which is good).
8. **Small ones**: `-g` in `RTFLAGS`/`IMGUIFLAGS` only costs binary size (the shipped binary is
   "with debug_info, not stripped"); CET is off (0 `endbr64`); there are no `assert()`s in runtime
   code, so `-DNDEBUG` would not help; `-O3` is not obviously better than `-O2` even in compile time.

---

## 8. Checked and already good (don't spend time here)

- Object handles are a single dereference; no handle tables, no per-call locks at the NVN layer.
- Shader translation is content-keyed, off the render thread, with in-memory + on-disk caches
  (`cache/programs.bin`, `cache/spirv/<hash>.spv`) and a prewarm list; not per frame.
- No per-command `malloc` in command recording; command dispatch is a switch.
- Descriptor pools are per-submission and reset on slot reuse (the right lifetime), not per draw.
- Pipelines are built on worker threads with a per-frame time budget, and `VkPipelineCache` is used
  for every creation and loaded from disk.
- Render passes and framebuffers are cached; no layout transitions per draw (everything lives in
  `GENERAL`); no per-frame MSAA resolve (lazy + shared).
- No validation layers, debug labels, `vkQueueWaitIdle`, or per-draw string formatting in release.
- Texture uploads are batched into one upload command buffer per submission, with no per-upload
  fence; no mipmap generation (guest mips are uploaded).
- Image views (16 k entries), framebuffers, samplers and the default sampler are cached; memory goes
  through one VMA allocator; destruction is deferred by submission number.
- FSR/scale resources are created once, and the whole FSR path is skipped when both are off.
- Screenshot PNG encoding runs on its own thread.
- The vertex copy cache already demonstrated the pattern (880 MB/s → 8 MB/s per `STATUS.md`).

---

## 9. Suggested order of work

1. **Startup/load (no gameplay behaviour):** §6.1 #3 (one move: arcmerge cached early-return),
   §6.1 #1 (symbol hash), §6.1 #2 (RomFS index cache), §6.1 #7 (drop the duplicate tree walk),
   §5.4 (adler32 kernel, inflate long-match `memcpy`, IPC zeroing).
2. **Free, no behaviour change:** §1.10 (`vk_collect_dead` early-out, getenv cache, stats timers),
   §2.12 (drop `fflush`), §5.2.1 (audio mix-buffer clear, one line), §7.4 (log buffering),
   §7.2 (`-march` for the runtime).
3. **Cheap and high value:** §1.2/§1.3 (constant blocks, index dedupe), §1.6 (quad expansion),
   §2.5/§2.6 (barriers around the scale blit, depth clear), §2.2 (SUBOPTIMAL), §2.3 (image clear),
   §6.2 #10/#11 (host loop gating, bind snapshot).
4. **The descriptor rework:** §1.1 + §1.7 + §3.4 (shared set layouts) + §3.5 (full-key compare).
5. **Platform:** §3.1/§3.2 write watching on Linux and the batched Windows path; §6.3 sleep spin.
6. **Measure, then decide:** §7.1 (`-O2` game code), §4.1 (non-aliasing memory pointer),
   §2.1 (barrier narrowing on the R9), §1.4 (pipeline key), §5.3 (HLE libc dispatch overhead).

---

## 10. How to verify each change

- Build: `make -j12 FFMPEG=system GAME=mhgu` (this checkout's usual invocation).
- Correctness: `make test-lift` (differential against Unicorn over both modules), `make test-simd`,
  `make test-settings`, `make test-armor-fusion`.
- Frame timing: `MHGU_VK_STATS=1` prints exec ms, passes, draws, copies, compiles and the slowest
  frame; `MHGU_CONTROL` `gpuprof` gives per-pass GPU timestamps; `MHGU_FNPROF` (live) and
  `MHGU_CONTROL` `fntrace` give per-function counts — the fastest way to size §4.
- A/B protocol: same save, same spot (e.g. `MHGU_QUICKHUNT=town`), same ini, 60 s, compare the
  `exec ms` line and the fence-limited fps; for GPU-side changes use `gpuprof` deltas because the
  fps may be capped by the game's own limiter.
- Keep a copy of the save and the ini while testing (the game writes on exit); use `MHGU_*`
  environment overrides rather than editing the user's ini.

### Caveats

Everything above is static analysis. Frequency figures are estimates derived from call sites and
the documented workload; no profiler was run for this audit. The two biggest levers (§4.1 and §4.2)
are hypotheses that need a prototype and a benchmark before anyone commits to them, and §2.1 has a
known visual-regression risk on GCN1 that only the R9 can settle.

---

## 11. Built and measured (2026-11-11)

**Rig:** Ryzen 5 5600X + Radeon AI PRO R9700, host display 2560x1440. The game rendered a
3840x2160 window (4K), `MHGU_QUICKHUNT=town`, VSync off, with the pipeline prewarm after the town
load excluded. At the requested 240 fps cap the frame rate cannot rise, so the metric there is CPU
time per presented frame (`/proc/<pid>/stat` utime+stime over 15 s ÷ frames). The headroom above
the cap was measured with `MHGU_BENCH_FPS=<n>`, a benchmark-only environment override in
`runtime/hle/vi.c` that replaces the engine's 240 fps clamp (`[vi] frame rate target <n> (benchmark)`).

| Build | 4K town, uncapped | 4K, 240 fps cap, CPU/frame |
|---|---|---|
| before (lifted code `-O1`) | 367 fps | 6.21 ms |
| lifted code `-O2` (§4.2) | 372 fps | 6.13 ms (−1.3 %) |
| `vk_tex` timing gated on `MHGU_VK_STATS` | 372 fps | 6.13 ms (stats runs only) |
| 200 µs → 20 µs final sleep spin | **404–407 fps** | **5.82–5.86 ms (−4.7 %)** |
| requested setup: gamescope 4K, 240 fps | — | **240.0 fps** median, min 239.8, σ 0.07 |

The sleep change is the win that matters: `plat_sleep_until_ns` busy-spun the last **200 µs** of every
sleep. At 240 fps the engine's limiter and the VI vsync thread sleep ~480 times a second together,
i.e. roughly 19 % of a core spent in `sched_yield()` loops; at a 500-target benchmark the spin kept
the vsync thread from feeding frames fast enough, which is why the uncapped rate jumped as well.
A 20 µs tail keeps sub-frame wake precision and leaves the 240 fps cap exact (σ 0.07 over 42 s).

Measured and reverted: a descriptor-set cache in `stage_set` (reuse when layout + contents match)
cost **+2 % CPU** — the town changes bindings too often for the signature to pay for itself. A
`memcmp` fast path for the pipe key was neutral. Removing the per-record `fflush` from the pipeline
records cache lost the file's tail on a killed process, so clean exit now flushes it explicitly.

`-march=x86-64-v2` on the runtime (`RTFLAGS`, `$(GENARCH)`): adopted, and measured after a forced
rebuild (a flag-only edit does not invalidate `.o` files — the sources were touched). 5.82 and 5.85 ms
CPU/frame at the 240 cap against 5.82/5.84 before: neutral, as `docs/AVX2-FALLBACK.md` predicted (v2
only re-encodes scalar code; the runtime's hot loops are branch/memory bound). It makes the executable
match the ISA floor that document already claimed for it. The game code has used `GENARCH` = v2 all
along.

Host-load caveat: the 240.0 fps gamescope figure above was measured on an idle host. With Spotify
holding ~2 cores (load ~4) the same build's gamescope presents settle at ~230 fps, while the game's
own limiter still reports 239.9 in the no-compositor harness — the shortfall is compositor pacing
under contention, not the game. Re-check fps claims on an idle machine.

Landed but below the noise floor (correct work, no measurable 4K-town fps effect): the const
zero/rscale buffer (one 68 KB allocation, never grown), index buffers through the per-submission
copy cache, the `vk_collect_dead` early-out, the smaller audio mix-buffer clear, the buffered log,
and the arc-merge cache early-out.

Still open: the biggest cost in the profile is the per-frame vertex re-copy (~10 MB/frame, ~10 % of
CPU in `memcpy`), which needs page write tracking on Linux (§3.1); without it the vertex cache stays
disabled there. Game code remains ~45 % of the profile, and `-O2` only moved it ~3 %.


### Follow-up pass: where the remaining CPU goes

A time-filtered `perf` profile of a 4K town run (samples from 35 s on, so boot and town-load are out)
gives: `draw` 8.1 %, `upload_texture` 5.9 %, `stage_set` 5.3 %, libc `memcpy`/`memset` ~15 %,
`libvulkan_radeon` 9.5 %, game code (38 % overall) spread over hundreds of lifted functions.

- **Texture upload is genuine steady-state work.** `MHGU_VK_TEXSTATS`-style counters (removed after
  the measurement) showed ~240 uploads/s of 1 MB linear textures (≈480 MB per 2 s), 336 MB/s of
  full hashing and ~5700 strided sample checks per 2 s. The 4 MB/frame hash budget only limits the
  periodic re-check of static textures; textures flagged dynamic are fully hashed every frame. That
  is the game genuinely regenerating a texture per frame, not a caching failure.
- **The per-draw max-index scan is memory-bound.** `perf annotate` put ~43 % of `draw`'s samples on
  the scalar scan of the index buffer; replacing it with an SSE4.1 (8×u16/4×u32) max scan changed
  CPU/frame by nothing measurable (5.89 vs 5.89 ms), so it was reverted — the loads, not the loop
  instructions, were the cost.
- Small re-orderings tested and reverted as neutral: a pause-loop spin (no `clock_gettime` per
  iteration), a single `memcpy` for contiguous linear texture rows (kept; one call instead of one
  per row), and zeroing only the used part of the 2 KB driver-constant block (kept).
- The host drifted from 5.87 to 5.93 ms CPU/frame over the session (~1 %), so anything below ~1 %
  is not measurable here. The two remaining large items are unchanged: per-frame vertex re-copies
  need page write tracking on Linux, and per-draw driver calls (`draw` + `stage_set` + the Vulkan
  driver ≈ 23 %) need batching or a descriptor-set design that does not rebuild every draw.


#### Second pass: a real hunt, and two more neutrals

- **A large-monster village quest (206) also holds 240 fps at 2160p** (steady-state samples 239.7–240.0,
  no frame-time spikes; exec 51 % of one core, GPU wait 0.3 %, ~845 draws/frame). The 16–29 ms slowest
  frames in both scenes are the start-up pipeline builds, not steady state. On this GPU the requested
  2160p/240 target is met in both town and a hunt, so remaining CPU work only buys headroom/power.
- **Fast-upload mode for per-frame dynamic textures** (skip the full hash while a texture changes on
  three consecutive checks, then upload unconditionally for eight frames and probe again): 5.86 ms vs
  5.88 ms CPU/frame, inside the host drift. Reverted rather than carry a heuristic that skips the HD-UI
  content checks for eight frames.
- **`-O3` for the lifted code**: 5.92 ms vs 5.92 ms at the same time of day; reverted to `-O2`.

The remaining structural options are the two from the first pass: page write tracking on Linux for the
vertex cache (this host has `vm.unprivileged_userfaultfd = 0`, so it would need an mprotect/SIGSEGV
scheme), and fewer per-draw Vulkan calls (`draw` + `stage_set` + driver ≈ 23 % of CPU in town).
