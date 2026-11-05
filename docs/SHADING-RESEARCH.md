# Shading research: what modern effects this renderer can support

Question: can MHGU get modern shading techniques, and in particular ambient occlusion, without a rewrite?

Short answer: yes for anything that can be computed from the **depth buffer** - ambient occlusion, contact shadows,
depth fog, depth of field, edge outlines, cheap screen-space bounce light. No for anything that needs the surface's
albedo and normals per pixel (PBR materials, real sun shadows, SSR, SSGI) unless the port starts producing its own
G-buffer by replaying the game's geometry. This document records the facts the answer rests on and a phased plan.

## What the renderer gives a post pass

Measured from a village frame (`MHGU_DUMP_DRAWS` + `MHGU_CONTROL "dump"`, 2560x1440 window):

| Data | Where it is | Notes |
| --- | --- | --- |
| Scene colour | the game's render target, NVN format `0x25` = RGBA8 UNORM | **display-encoded, LDR**: the game writes its final-looking values, no linear HDR pass |
| Scene depth | the render-size depth target, `D32_SFLOAT_S8_UINT` | the game's depth24s8 is emulated in 32-bit float: precision is not a problem; MSAA depth is resolved to a sampleable copy on demand (`vk_ms_resolve`, exec.c:1312) |
| Shadow map | pass 000, 1024x1024 depth-only, ~100 draws | the game's own sun shadow map; no light matrix captured |
| Camera | `CamRecord` (camera_jitter.c): `view[16]`, `proj[16]` (row-vector, m00/m11/m22/m23 known), near/far | hooks are installed only with FSR or `MHGU_JITTER_TEST` today |
| Frame structure | pass 003 = the 3D scene at render size; passes 004+ = the game's own post chain; then the HUD draws into the presented image | the scene target *is* the presented image: the HUD is drawn on top of it |
| Motion vectors | only while FSR runs | not available for TAA-style effects otherwise |

There is **no G-buffer**: no albedo, no normals, no material IDs. The game's materials are register-VM shaders with
baked lighting (see the earlier material experiments), so a post pass sees only colour + depth.

## The techniques, ranked

### 1. Ambient occlusion (the headline) - feasible
Depth-only SSAO in its modern form: **GTAO** (horizon-based, cosine-weighted, "Practical Realtime Strategies for
Accurate Indirect Occlusion", Jimenez/Wu/Pesce/Jarosz 2016) or the cheaper **HBAO** style. Both need only depth and
the camera, and both reconstruct normals from the depth gradient.

- Inputs: depth, `proj` (m00/m11 for view position, m22/m23 or near/far for linear z), screen size.
- Cost at 1440p: half resolution, 8 directions x 4-6 steps, plus a depth-aware 5x5 blur and a full-res composite:
  roughly 0.3-0.8 ms on a modern GPU; full resolution 1-2 ms.
- Quality risks, all standard: halos on depth discontinuities (fix: normal-aware radius, bilateral blur), the sky
  (depth == 1: skip), the AO applied to an already display-encoded image (multiply as-is, or decode to linear and
  back at a small cost).
- The AO would also darken the HUD if it runs at present. The clean place to apply it is the moment the HUD starts
  drawing into the presented image - FSR already has exactly that hook (`hud_start`, fsr.c:1664) for its own reasons.

### 2. Depth fog / aerial perspective - feasible, cheap
Linear depth from the camera, a per-area colour and density; hides the AO horizon and gives open maps the haze the
game's own shaders bake in. A handful of lines, one full-screen pass.

### 3. Contact / screen-space sun shadows - feasible with a caveat
Ray-march the depth buffer toward a light direction from each pixel. Needs a **sun direction**, which the port does
not capture; it would be an artistic setting (or read from the game's shadow pass later, which needs its light
matrix). Works best layered with AO; cost similar to AO.

### 4. Edge outlines (toon ink) - feasible, cheap
Depth + normal discontinuity detection gives MHGU-style ink lines. Fits the game's art direction better than most
"modern" effects and costs a fraction of AO.

### 5. Screen-space bounce light (SSGI-lite) - feasible, stylised
AO plus a blurred, occlusion-modulated colour bleed from neighbouring pixels. Not physically based, but it adds the
colour bounce the baked lighting lacks.

### 6. Bloom, tonemap, grade, DOF, vignette - feasible, post only
The Mellow chain was exactly this (reverted by request). Any of it can come back as a normal post pass; DOF needs
depth, the rest does not.

### 7. PBR materials, real-time sun shadows, SSR, SSGI, TAA - not feasible from depth alone
They need albedo/normals/motion vectors. Producing them means re-rendering the game's geometry with the port's own
vertex shaders and a G-buffer - the draw-replay project. The per-material attempt showed the smaller version of this:
finding each material, decoding its varyings, and reimplementing it loses the game's fog, reflections and shadows,
and has to be repeated for every material family (terrain worked; buildings and characters were not cracked before
the attempt was reverted).

## Data we would add for AO

1. **Scene depth tracking in exec.c.** FSR already does this (`vk_fsr_draw`, fsr.c:1706: four render-size depth
   targets ranked by depth-writing draws), but only when FSR is on. A small parallel note in the draw hook (a few
   lines) gives the same for the AO setting: remember the render-size depth target with the most depth writes this
   frame.
2. **Camera capture.** `camjit_init` installs the view/projection hooks only for FSR today; add the AO setting to its
   gate. Capture without jitter is what we want (the jitter itself is FSR's, off unless FSR runs).
3. **A scene-post hook.** The presented target is known at present (`backend_present`), but the HUD has been drawn by
   then. Two options:
   - phase 1: run AO at present over the final image (the HUD gets AO too - visible on text over geometry);
   - phase 2: run it at the first HUD draw into the presented target, the way FSR's `hud_start` does, so only the
     3D picture is occluded.
4. **The pass itself** (`runtime/nvn/vulkan/ao.c`): half-res AO image (R8), depth-aware blur, composite. The port
   already has everything needed (`vk_compile_glsl`, `vk_render_pass`, `vk_image_view`, `vk_alloc_set`, the descriptor
   conventions), as the Mellow chain demonstrated.

## FSR and MSAA

- **MSAA**: the depth is resolved to a single-sample copy when read (`vk_ms_resolve`); run AO after the resolve.
- **FSR**: with FSR on, the presented image is the *upscaled* one and the depth is render-size. AO should stay at
  render size and be folded into FSR's prep (before upscaling), or sample the render-size depth from the output size
  (bilinear, slightly soft). Phase 1 can simply require FSR off, or run at output size.

## Plan, if we build it

| Phase | What | Effort |
| --- | --- | --- |
| 1 | depth tracking + camera gate + AO pass + composite at present, `[Graphics] AmbientOcclusion` setting and row | 2-4 days, testable in the village and a hunt (quickhunt) |
| 2 | move the composite to the HUD start so the HUD is untouched; FSR path at render size | +1-2 days |
| 3 | depth fog, then optional contact shadows / ink outlines | +1-3 days each |
| 4 | (only if wanted) draw-replay G-buffer for material-level shading | weeks-months |

Phase 1 is the right first slice: it is self-contained, it exercises every piece of plumbing a later pass needs
(depth, camera, half-res pass, composite, setting), and it can be judged in a screenshot before anything else is
built on it.

## References

- GTAO: J. Jimenez, X. Wu, A. Pesce, W. Jarosz, *Practical Realtime Strategies for Accurate Indirect Occlusion* (2016).
- HBAO: L. Bavoil, M. Sainz, *Image-Space Horizon-Based Ambient Occlusion* (2008); NVIDIA HBAO+ talks.
- Intel XeGTAO (open implementation of GTAO for depth-only AO).
- ReShade's MXAO for the depth-only, post-process variant and its artifact mitigations.
- *Scalable Ambient Obscurance* (McGuire et al.) for the depth-gradient normal reconstruction.
