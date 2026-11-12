# ADR 0008: Owning the 3D renderer in Vulkan — scale, HD composite, and post-FX

Date: 2026-11-12
Status: Proposed (slices staged; slice V1 designed against the current sink)

## Context

The user goals, in their words: own the renderer, beat the OpenGL compute
renderer's performance, and add shadow maps, bloom, zoom and pan. The OpenGL
compute lane (ADR 0007 amendment, `--renderer3d compute`) ships hi-res 3D today
from vendored code; it stays as an oracle and fallback. Our own Vulkan sink
(`render/src/ds/`, ADR 0007) is slice 1: a faithful 256x192, 1x rasterizer on
the `Accelerated=false` contract.

What the sink actually is, from reading it:

- All sizes are baked constants: `kWidth=256`, `kHeight=192` in
  `render/src/ds/ds_3d.cpp`, and `#define ScreenWidth 256 / ScreenHeight 192 /
  RenderScale 1` in `tools/ds_shader_gen.cpp:325-327`, compiled to SPIR-V at
  configure time by glslang (`cmake/Shaders.cmake`).
- The CPU builds Y-spans (`setup_y_span`, `build_spans`) from
  `Polygon::Vertices[i]->FinalPosition` — native screen coordinates.
- The runner emits `NdsGpu2dHdFrame` (the 2D stack with the 3D layer removed,
  `runner/src/gpu2d.h`) whenever the sink claims to consume it
  (`pkmng5pc_ds_render_consumes_hd()`, currently 0) and `NDS_INTERNAL_RESOLUTION`
  is above 1.
- `verify` mode (PKMNG5PC_DS3D_VERIFY=1) diffs the Vulkan frame against the soft
  renderer per pixel — the correctness oracle for every change below.

The vendored GL renderer is the reference for each feature:
`ComputeRenderer::SetRenderSettings(scale, highResolutionCoordinates)` rebuilds
its shaders with `ScreenWidth = RenderWidth * ScaleFactor`, the CPU span setup
multiplies positions by the factor
(`GPU3D_Compute.cpp:737-741: FinalPosition[0] * ScaleFactor`), and
`ComputeHost.cpp` composites the hi-res 3D into the 2D stack on the GPU.

## Decision

### Slice V1 — scale-aware Vulkan sink (SHIPPED 2026-11-12, readback amended)

The sink gains a raster scale (integer 1..8) fixed at renderer init.
**Status: implemented and verified** — spec-constant shaders, runtime surface
geometry, upstream's hires-coordinate span scaling, 4:3 gate lifted (any even
256..448 px whose product with the scale is a 64-multiple; 16:9's 342 stays
faithful-only), scale via `PKMNG5PC_RENDER_SCALE` forwarded from
`-RenderScale`. Verified: scale-1 verify color 0-diff; **the faithful readback
is bit-identical at 2x/6x** (block-boundary point-sample); band 448 at 2x =
896x768 raster through Native present.

**Amendment (same day): the box-filter SSAA readback was tried and reverted.**
Averaging the hi-res surface down to 1x *before* the CPU composite blurs the
2D stack composited over the 3D layer — semi-transparent sprites and 3D/2D
boundaries sample muddy blocks, and the SSAA result at 1x composite reads as
blur, not resolution. The user-visible lesson: the hi-res benefit only exists
where the blend happens at the same density as the raster, i.e. the V2 GPU
composite. The faithful readback point-samples the block-boundary plane
(bit-identical at every scale, verify-proven), and the hi-res `FinalFB`
surface stands ready for V2.

- **Baked per-scale shader variants, not runtime compilation.** Vulkan has no
  runtime GLSL compiler here and the project ships no runtime glslang; the
  generator already emits per-shader SPIR-V at configure time. It gains a scale
  loop (1..8): 33 shaders x 8 scales = 264 small modules, same source, only the
  three defines differ. Vulkan specialization constants were considered and
  rejected: the vendored sources use the constants as int literals in mixed
  signed/unsigned arithmetic (`int resultOffset = ... * ScreenWidth` vs
  `const uint ResultDepthStart = ...`), and relying on glslang's implicit
  folding across every one of 33 shaders is exactly the kind of fragile the
  verify harness cannot fully cover.
- **Runtime geometry.** `screen_width = RenderWidth * scale`,
  `screen_height = 192 * scale` become Impl members; buffers, images, tile
  counts, bin dispatches and the final-pass dispatch derive from them at init.
  The 4:3-only gate in `runner_bridge.cpp` lifts: `RenderWidth` comes from
  `GPU3D::GetRenderWidth()` (256..448, any even value — the CPU span setup has
  no tile-multiple constraint, unlike the GL shaders' baked-width dispatch).
- **CPU span scaling matches upstream exactly**: positions multiply by scale
  (`FinalPosition * scale`) before the span math; every clamp and increment is
  derived from the scaled coordinates, as `GPU3D_Compute.cpp:737-741` does.
- **Readback.** The hi-res color/attr images box-filter down to the 1x readback
  buffers `GetLine()`/`GetAttrLine()` serve. Box-filtering (not point-sampling)
  is deliberate: it makes scale>1 a *visible* supersampling mode through the
  existing faithful present path — anti-aliased 3D at 1x cost of one small
  compute pass — while scale=1 stays bit-identical, so the digests and the
  byte-lock survive. The verify harness compares only at scale 1 (at scale>1 the
  filter is the difference).
- **Control.** `NDS_INTERNAL_RESOLUTION` (already parsed end to end) drives the
  scale on the Vulkan lane; `tools/play` already forwards `-RenderScale` into
  it when the sink is on. The TUI's "3D internal resolution" row stops saying
  2x-4x fall back to the CPU compositor.

Gate: `verify` at scale 1 stays 0-diff; at 2x/4x the downsampled frame differs
from soft only inside the filter footprint; frame digests at scale 1 unchanged.

**2026-11-12 amendments:** the sink pads its raster width up to the next
64-px tile multiple (the guest band and readback keep the real width), so
**16:9 (342 px) now composites at any scale** — the old refusal is gone. Scale
is 1..8 on the sink lane; the compute lane's parser caps at 4, so the TUI caps
its value there and the command clamps with a note. The resolution row is
disabled when neither GPU lane is active (the faithful soft renderer is 1x by
nature). A present filter (`nearest`/`linear`, `PKMNG5PC_PRESENT_FILTER`)
selects the final window scaling — linear is the smooth-but-crisp look
melonDS's screen scaling gives.

### Slice V2 — Vulkan HD composite (SHIPPED 2026-11-12, minimal)

**Status: implemented and running.** The presenter gains `present_hd()`: a
compute composite (`render/src/present_hd.comp`, a faithful port of the
OpenGL lane's `ComputeHost.cpp` fragment math — blend16/blend5, the
(priority, order) 3D insert, BLDCNT effects, master brightness) that blends
the sink's hi-res `FinalFB` surface into the no-3D stack
(`NdsGpu2dHdFrame`, emitted by the runner once
`pkmng5pc_ds_render_consumes_hd()` reports a consumer) at the placed
rectangle's own resolution, then blits it into the swapchain. The rectangle
uses fit scaling, so the hi-res detail is visible; the bottom band and the
bottom window come from the faithful 1x framebuffer (no 3D there, so the
2D stays pixel-crisp — the V1 blur lesson, fixed where it belongs).

`HdComposite` carries zoom/pan (push constants already honour them);
native_present defaults zoom 1.0 — the env wiring, the separate-layout HD
band, free resolution from the window size, and a pixel-exact A/B against
the OpenGL lane (the window-capture harness is DPI-unstable between runs)
are the remaining follow-ups. Performance at high scale is still raster-bound
(N² work, serialized fence) — the async-compute and readback-elimination
items below are where the 4060-class headroom lives.

The sink exports its hi-res color image (and attr); the presenter learns a
graphics pipeline (the first shader in `render/` — ADR 0005's shader-free
presenter is amended) that composites, per output pixel:

1. sample the hi-res 3D color,
2. sample the upscaled 2D stack (`NdsGpu2dHdFrame` top/below pairs, priority,
   order, BLDCNT alpha — the exact `ComputeHost.cpp` algorithm ADR 0007
   decision 5 named),
3. apply master brightness.

`pkmng5pc_ds_render_consumes_hd()` returns 1, the runner's HD emit switches on,
and the canvas becomes hi-res. The window-size/present-scaling controls from
ADR 0007 amendment carry over to the hi-res canvas.

**Zoom and pan land here**: the composite transform (scale + offset in output
pixels) is a push-constant, so Play gains zoom (magnify the DS image past
letterbox-fit) and pan (look around a magnified image); the pointer mapping
inverts the same transform, so touch stays exact under magnification.

**Free resolution (separate layout)**: with the composite owned by us, the
scale stops being a small integer list — the sink rasterizes at whatever
density the window needs (`ceil(window_px / 192)` capped by a max), rebuilding
the pipeline set when the window crosses a density step. Separate layout only:
one window, one screen, one density; stacked keeps integer scales so both
screens share one canvas.

Gate: HD composite output matches the GL lane's HD output on the same frames
(swap screenshots through both lanes at identical settings); digests at the
native surface unchanged.

### Slice V3 — bloom (and the texture-upscale port)

**2026-11-12 note:** xBR texture upscaling ships for the *compute lane* first
(the runner's vendored `TextureUpscale.cpp`, wired through `-TextureUpscale`
1/2/4 → `NDS_TEXTURE_UPSCALE`, the TUI row and both play scripts). It is what
explains the melonDS-vs-ours billboard comparison: melonDS smooths the player
quad's texture; our lanes uploaded native texels. Porting the same xBR-lv2
shader (MIT, `TextureUpscale.cpp`) into the Vulkan sink as an upload-time
compute pass — the sink's texture store already samples with normalized UVs,
so the upscaled arrays need no shader change — is the sink-lane follow-up;
bloom stays below it.

Compute post-pass on the hi-res 3D surface before the composite: bright-pass at
a threshold, separable blur (two 1D dispatches), additive blend in the
composite with an intensity push constant. Play toggle + threshold/radius
controls. Runs at hi-res so glow is smooth; cost is three small dispatches on
an at-most-2048x1536 surface.

### Slice V4 — shadow maps

The GE has no light matrices, so this is the first *non-faithful* geometry
feature and it needs its own design pass before implementation:

- a second geometry pass rendering polygon depth from a configurable light
  direction into a depth atlas (the tile pipeline makes this cheap — same bin,
  new raster mode),
- a light-space transform and bias strategy (the DS W-buffer is logarithmic;
  shadow acne needs depth-offset tuning per scene),
- a Play-page control (light direction + strength, off by default).

Blob shadows in the games keep working; shadow maps add directional shadows on
top. Design first, implement after V2/V3 are stable.

### Performance (continuous, owned)

The sink is ours, so the performance levers are ours too: timeline semaphores
and a second queue to overlap binning with raster, dropping the readback when
the HD composite consumes the image directly (V2 removes the 1x readback from
the presented path entirely), a disk pipeline cache, and profiled tile-size
tuning. Every change keeps the scale-1 verify oracle green.

## Consequences and risks

- The GL compute lane stays supported untouched ("don't remove the option") and
  doubles as the reference implementation for V2's composite math.
- The faithful contract is preserved at scale 1 only; scale>1 changes the
  faithful readback by design (documented, and the coverage/capture tools pin
  scale 1).
- The first shaders enter `render/` (V2): ADR 0005's shader-free presenter is
  amended, not abandoned — presentation-only, no game-logic coupling.
- Shadow maps are the largest design risk and are explicitly last.

## References

- ADR 0007 (native DS render sinks; decision 5 is the HD split, decision 6 the
  Vulkan port this ADR stages)
- `runner/src/gpu2d.h` `NdsGpu2dHdFrame`, `runner/src/melonds_compute/ComputeHost.cpp`
  (the composite algorithm to port)
- `render/src/ds/ds_3d.cpp`, `tools/ds_shader_gen.cpp` (the sink)
- `re/notes/wide-output.md` (wide measurement; the per-title frustum patch is
  the edge-objects fix, tracked there)
