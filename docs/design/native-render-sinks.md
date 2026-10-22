# Native DS render sinks - architecture and slice plan

**Status:** design for `docs/adr/0007-native-ds-render-sinks.md` (accepted
2026-10-21 as an early M8 spike). Slice 0 infrastructure and the Slice 1 3D sink
are implemented; the 2D sink is not. Default off; this is not M6 or M8
completion.

## 1. Scope

Replace two host-side rasterization endpoints with Vulkan work while keeping the
guest GPU device model authoritative:

- the **3D sink** replaces melonDS's `Renderer3D` rasterizer;
- the **2D sink** replaces the per-scanline 2D compositor that produces the
  final screen image.

The 3D sink lands first. The first visible slice still uses the CPU 2D
compositor and the existing `NdsGpu2dHdFrame` description; the 2D sink is a
second, larger slice. The alpha targets the top screen first; the bottom screen
stays native until its policy is audited.

In scope: high-resolution 3D, true scene widening, selectable aspect ratios,
one alpha master toggle, CPU fallback, verification, and the build/patch
infrastructure those need.

Out of scope: post-FX (bloom, shadow maps), frame decoupling/interpolation,
New Game+/Nuzlocke, D3D12, Android/web, and any change to guest-visible state
layout.

## 2. Current path and seams

```
guest writes
  -> GPU3D geometry decode (vendored, unchanged)
  -> Renderer3D rasterizer (CPU soft today)
  -> GPU2D scanline compositor (CPU today; adaptive/HD paths exist)
  -> frontend canvas / native presenter
```

Relevant seams:

- `GPU3D::SetCurrentRenderer(std::unique_ptr<Renderer3D>&&)` is the only
  install point. There is no `nds_gpu3d_set_renderer`.
- `Renderer3D` is in `third_party/ndsrecomp/runner/vendor/melonds/GPU3D.h` and
  exposes `Reset`, `SetRenderWidth`, `RenderFrame`, `GetLine`, `GetAttrLine`,
  `VCount144`, `Stop`, `RestartFrame`, `Blit`, `SetupAccelFrame`,
  `PrepareCaptureFrame`, `BindOutputTexture`, `NeedsShaderCompile`,
  `ShaderCompileStep`.
- `NdsGpu2dHdFrame` (`runner/src/gpu2d.h:125`) is the existing 2D-to-presenter
  contract: `top_pixels`/`below_pixels` as RG32UI pairs at `width * 192`,
  `width`, `priority_3d`, `order_3d`, `bldcnt`, `master_bright`, `eva`, `evb`,
  `evy`, `render_xpos`.
- `ComputeHost.cpp:667-731` already consumes that frame and re-inserts the
  hi-res 3D surface with the DS blend rules. It is the algorithm to port.
- `render/src/vulkan_presenter.cpp` owns a private instance/device/queue and
  the swapchain. `render/src/native_present.cpp` is the C bridge the frontend
  calls after creating its windows.
- `frontend.cpp:1931` calls `pkmng5pc_native_present_init()`;
  `frontend.cpp:2004` and `frontend.cpp:1767` gate
  `nds_gpu2d_set_hd_emit()`.

The emulation loop and presentation run on the same frontend thread
(`scheduler_run_round()` in the frontend loop). That is the synchronization
model this design relies on.

## 3. Target architecture

```
guest writes
  -> GPU3D geometry decode (vendored, unchanged)
  -> 3D sink (Vulkan compute; low-res + hi-res)
       | low-res color/attr readback
       v
  -> GPU2D decode + CPU compositor (first slice)
       -> NdsGpu2dHdFrame
  -> HD presenter (Vulkan; inserts hi-res 3D, blend, scale/letterbox)
  -> swapchain

later:
  -> GPU2D decode (CPU per-line) -> 2D sink (Vulkan; layer select,
     windows, blend, 3D insertion, output_width x 192)
  -> presenter (scale/letterbox only)
```

Two independent fallback paths:

- **3D fallback:** the bridge keeps a CPU soft renderer and swaps it into
  `GPU3D` for a failed frame (or until relaunch), so the CPU 2D path still has
  valid lines.
- **2D fallback:** if the 2D sink fails, the CPU adaptive compositor still
  produces the native/`NdsGpu2dHdFrame` output; the presenter presents the CPU
  surface.

Display capture and verify mode force the CPU path; the sinks are bypassed for
those frames. This keeps guest-visible VRAM identical.

## 4. Components and file map

```
render/src/vulkan_context.{h,cpp}        shared instance/device/queue/allocator
render/src/vulkan_presenter.cpp          swapchain owner + DS composite hook
render/src/native_present.cpp            lifecycle C bridge
render/src/ds/ds_3d.{h,cpp}              Renderer3D implementation (Vulkan compute)
render/src/ds/ds_shaders.txt             shader list: name|source symbol|defines
render/src/ds/runner_bridge.cpp          C ABI: install/teardown/fallback/HD frame
render/include/pkmn/render/ds_frame_policy.h  CPU fallback policy
tools/ds_shader_gen.cpp                  GLSL variant generator (build tree)
cmake/Shaders.cmake                      glslang FetchContent + SPIR-V embedding
patches/ndsrecomp/0003-native-render-sink.patch
patches/ndsrecomp/0004-gpu3d-device-accessor.patch
```

Build targets:

- `pkmn_render` keeps `backend.h`, `Presenter`, `compose_screens()`, the Null
  presenter and `VulkanPresenter`.
- `pkmn_render_ds` links `pkmn_render`, the runner headers it needs, and the
  generated shader sources. It is linked into `nds_runner` by
  `cmake/Runner.cmake` when `PKMNG5PC_NATIVE_RENDERER=ON` (default OFF; the
  runtime toggle also remains off). With the option OFF, the target, glslang and
  the patch's sink calls are absent; the runner is the pre-spike binary.
- `shader_sources` is an object library generated at build time. No SPIR-V or
  generated C++ is committed.

`pkmn_render` stays free of runner/game headers. Only `render/src/ds/` and
`native_present.cpp` include the vendored runner types.

## 5. Data contracts

### 5.1 Low-res 3D readback

The sink returns the same `u32` line layout as the soft renderer: RGB6 channels
at bits 0/8/16 and the attribute/alpha byte at 24. `GetLine(line)` returns a
pointer valid until the next `RenderFrame`; `GetAttrLine(line)` returns the
coverage/polygon-id line used by the wide compositor. Both are backed by a
host-visible, coherent readback buffer sized `width * 192 * 4`.

The sink waits on the readback fence inside `RenderFrame` (the CPU 2D
compositor needs the line before the next scanline batch). This is a deliberate
stall; the design is still faster than the CPU rasterizer. Double-buffering the
readback is a later optimization, not a contract change.

### 5.2 Hi-res 3D surface

The sink owns a device-local `R8G8B8A8_UNORM` image at
`(width * scale) x (192 * scale)`, where `scale` is
`nds_gpu3d_internal_scale()` (1..4). The HD composite samples it with the
unscrolled `render_xpos` semantics from `NdsGpu2dHdFrame`; the CPU wide line
keeps its scrolled semantics. The image is never read by the guest and never
enters a savestate.

The compute port keeps its 8x8 tile scheme. At widths that are not multiples of
8 (for example 342), the internal tile grid rounds up and the padding columns
are discarded; the visible width is still the requested even width. This must
be covered by the width audit in slice 3.

### 5.3 2D input (first slice)

The first slice consumes `NdsGpu2dHdFrame` unchanged. The CPU adaptive
compositor resolves the 2D stack with the 3D layer removed and writes the top
and below pairs at `width`. The HD shader re-inserts 3D using
`(priority_3d, order_3d)`, applies `BLDCNT`/`EVA`/`EVB`/`EVY` and master
brightness, and scales to the destination rectangle. This is a direct port of
`ComputeHost.cpp`, not a new blend model.

### 5.4 2D sink descriptor (slice 4)

The 2D sink needs the layers that the CPU compositor currently collapses into
top/below. The CPU keeps the tile/OBJ decode and emits, per scanline:

- four BG layers: color/alpha, priority, order, BLDCNT target bit, window
  effect bit, valid;
- one merged OBJ layer (the CPU already resolves OBJ-vs-OBJ priority/order), in
  the same fields;
- the backdrop color;
- the window mask (WININ/WINOUT/OBJ-window) and the effect-enable bit;
- `BLDCNT`, master brightness, `EVA`/`EVB`/`EVY`;
- the 3D surface, `priority_3d`, `order_3d`, `render_xpos`.

The GPU performs the same total order as `compose_line6`: select top and below
by `(priority, order)`, insert 3D, apply windows and blending, then master
brightness. The shader uses integer math (`RGBA8`/`RG32UI`), not floating point,
so bit-exactness is achievable. The CPU still owns the decode tables, palette
lookup, VRAM/OAM reads, and all guest-visible state.

The descriptor is a per-frame structured buffer in host-visible memory. It is
built on the frontend thread after decode and consumed by the 2D sink before
present. The exact struct layout is frozen with the shader in slice 4; the
invariant is that every field `compose_line6` reads crosses the boundary and no
guest pointer does.

### 5.5 Texture cache

The compute port keeps the existing `Texcache` decode logic and adds a Vulkan
loader: decoded texels are uploaded to a device-local image (atlas or array),
with the same invalidation triggers as the GL loader. The cache is a host-side
derivative of VRAM; eviction never writes guest VRAM and never affects a
savestate.

## 6. Threading and frame lifecycle

All Vulkan calls happen on the frontend thread:

1. `VCount144`: sink latches per-frame state.
2. VBlank / `VCount215`: `GPU3D` calls `RenderFrame`; the sink submits compute,
   copies low-res color/attr to the readback buffer, and waits for the fence.
3. The CPU 2D adaptive compositor runs on the frontend thread (its band pool
   workers only touch CPU buffers), produces the HD frame, and the presenter
   records the composite.
4. Present, then the next frame.

The band-pool workers never touch Vulkan, and the audio thread is independent.
One queue is safe; no timeline semaphores are required for the first slice.
Savestate save/load and display capture flush the sink and invalidate the HD
frame before the next frame. A device-lost or out-of-memory result marks the
sink failed, swaps in the CPU renderer, and reports once.

## 7. Widescreen and safe area

### 7.1 3D

`nds_gpu3d_set_output_width(width)` sets the render width; the per-title policy
enables `GuestWideProjection`, which maps the guest viewport linearly across
the widened surface. The sink renders the full width; the 2D compositor reads
`nds_gpu3d_wide_line()`/`nds_gpu3d_wide_attr_line()`.

### 7.2 2D

The native 256-pixel view is centered at `extra = (width - 256) / 2`:

- text/affine/extended BG sampling uses `x_native = x - extra`;
- OBJ positions use `x_native = x - extra`;
- a window edge outside the native region clamps to the nearest native edge;
- HUD layers are anchored to the 4:3 safe area, reusing the existing
  `g_adaptive_center_native` / HUD-band logic where it applies;
- skybox repair stays a per-title option and must not silently disagree with
  the hi-res 3D surface; frames that need CPU-side repair fall back to the CPU
  composite (as the current HD path already does).

### 7.3 Aspect selection

| Label | Source size | `extra` | Status |
| --- | --- | --- | --- |
| 4:3 | 256x192 | 0 | first |
| 16:9 | 342x192 | 43 | first |
| 21:9 | 448x192 | 96 | first |
| 32:9 | 682x192 | 213 | deferred |

342 is the nearest even integer to 16:9 at 192 rows. `nds_gpu3d_set_output_width`
already accepts any even 256..448; the parser and config validator must be
widened (see section 11). 32:9 requires raising every 448 assumption:
`ScrolledLine[448]`, `ScrolledAttrLine[448]`, `kMaxAdaptiveWidth`,
`kComputeMaxWidth`, `FramebufferCPU[448*192]`, and the compute tile buffers.
That is deliberately not in the first alpha.

The selected ratio is a *source* width. The presenter fits the source rectangle
into the window with integer scaling where possible and letterboxes/pillarboxes
otherwise. It never stretches.

### 7.4 Per-title policy

The alpha reads a small data file (for example
`re/config/widescreen.toml`) selected by `PKMNG5PC_TITLE_ID`. `tools/play.ps1`
already reads the ROM game code for its default-ROM lookup, so the CLI can set
the title id from that (`IRBO` -> `black`, and so on) or accept `-Title`. The
shipping launcher will own the equivalent file later; the renderer must not
depend on `re/` at runtime in a release build.

```toml
[default]
screens = "top"          # none, top, bottom, both
max_width = 448
safe_width = 256
hud_anchor = "center"
guest_wide_projection = true
skybox_repair = false

[black]
screens = "top"
scene_fallbacks = ["menus"]   # scene list is audited, not guessed
```

The policy caps the TUI width. If the TUI selects 21:9 but the title caps at
342, the effective source is 342 and the presenter letterboxes; the stats
report the effective width and the reason. A title without a policy stays at
256. Black is the first audited title; White and B2/W2 inherit `[default]`
until audited.

## 8. TUI and launch surface

Existing pieces: `Settings` (`tui/model.py:485`), `play_command`
(`tui/commands.py:264`), the Play page (`tui/app.py`), and
`tools/play.ps1 -Native`.

Add to `Settings`:

```python
render_native: bool = False   # master alpha toggle
render_scale: int = 1         # 1..4, 3D internal resolution
render_aspect: str = "4:3"    # 4:3, 16:9, 21:9
render_verify: bool = False   # advanced/debug only
```

Add `PLAY_ASPECTS = ("4:3", "16:9", "21:9")` and `RENDER_SCALES = (1, 2, 3, 4)`
to `tui/model.py`. The Play page gets three rows:

- **Native renderer (alpha):** off/on. The help text says it replaces the DS
  rasterizer and implies native presentation; this is distinct from the
  existing native-present toggle.
- **3D internal resolution:** 1x/2x/3x/4x.
- **Aspect ratio:** 4:3/16:9/21:9, with the nominal source width shown
  (256/342/448). 32:9 is listed only after the cap work lands. The runner
  reports the effective width after the per-title cap.

When `render_native` is on, `play_native` is forced on (the Vulkan window and
presenter are required). The Play command adds `-Native`, `-Gpu`, `-Renderer
native`, `-Aspect` and `-RenderScale`; the play scripts translate those flags
into the environment values below. At 4:3 the scripts leave
`NDS_ADAPTIVE_WIDESCREEN` unset because slice 1 renders at the native 256-wide
framebuffer.

```
PKMNG5PC_RENDERER=native
PKMNG5PC_TITLE_ID=black         # selected title profile
PKMNG5PC_WIDESCREEN_SUPPORTED=top
NDS_ADAPTIVE_WIDESCREEN=top     # only when the aspect widens past 256
NDS_WIDESCREEN_WIDTH=342        # from render_aspect, capped by policy
NDS_INTERNAL_RESOLUTION=3       # from render_scale
```

`PKMNG5PC_WIDESCREEN_SUPPORTED` is the per-title capability the runner checks
before its fail-closed adaptive test; `NDS_ADAPTIVE_WIDESCREEN` is the request.
The TUI sets `supported=top` for the Gen 5 alpha; the bridge's policy caps the
width and can opt out. The stats report the effective width and the reason.

`tools/play.ps1`/`tools/play.sh` gain `-Renderer <off|native|3d|2d|both>` and
`-Aspect <4:3|16:9|21:9>` for CLI parity; `native` is the master toggle and
`3d`/`2d`/`both` are the debug switches. The TUI may pass the flags or the env
directly. If the binary was built without `pkmn_render_ds`, the TUI checks the
build cache (`PKMNG5PC_NATIVE_RENDERER=ON` and `PKMNG5PC_NATIVE_PRESENT=ON`) and
disables turning it on; if the saved setting is still on, Play refuses to launch
with a rebuild-or-turn-off message instead of silently omitting it. A hand-set
env makes any runner built with the native-present hook exit with a
rebuild-or-unset message. It never relaunches silently without the toggle.

The Play-page toggle is the master switch. `PKMNG5PC_RENDERER` is `off` by
default, `native` for the master, and `3d`/`2d`/`both` only for debugging;
`PKMNG5PC_DS3D_VERIFY=1` is advanced/debug only. None of the debug switches
are exposed as normal Play-page controls.

## 9. Staging plan

Each slice has a gate. A slice is not done until its gate is measured and
recorded.

### Slice 0 - infrastructure, no behavior change

- Extract `VulkanContext` from `vulkan_presenter.cpp`; presenter behavior and
  `backend.h` unchanged.
- Add pinned `glslang` via FetchContent, `cmake/Shaders.cmake`, one trivial
  shader that round-trips through the embedding step, and `pkmn_render_ds`.
- Add `patches/ndsrecomp/0003-native-render-sink.patch` with the guarded install
  call, HD-emit condition, width parser range and `adaptive_supported` hook.
- **Gate:** default builds unchanged; soft-path frame digests identical; the
  runtime-off run is byte-identical; with the build option ON a missing shader
  toolchain fails configure loudly, and with it OFF glslang is not fetched and
  the target is absent.

### Slice 1 - 3D sink at native resolution

- Vulkan compute rasterizer, low-res only, scale 1, width 256.
- `Accelerated=false`, `GetLine`/`GetAttrLine` readback, install via
  `SetCurrentRenderer`, CPU fallback swap.
- **Gate:** 3D color at most 1 LSB from soft at native; attr/coverage exact;
  guest RAM/save identical; display capture and savestate round-trips pass;
  no regression with the toggle off.

### Slice 2 - hi-res 3D and HD presenter

- Scale 1..4, low-res + hi-res outputs, `NdsGpu2dHdFrame` consumption, Vulkan
  HD composite, scale/letterbox.
- **Gate:** native 1-LSB tolerance holds at 1x; visible HD at 2x/3x; 1080p/60
  at 3x 3D with the CPU 2D path; per-frame fallback works; no HD emit when the
  presenter cannot consume it.

### Slice 3 - wide 3D and TUI ratios

- Accept even 256..448 in the parser/config; mark the native path as
  top-screen capable; per-title policy; 342 and 448; TUI selector.
- Audit the compute width rounding at 342 and the 2D wide paths for Gen 5.
- **Gate:** 16:9 and 21:9 Black runs with the correct safe area; no HUD
  clipping; effective-width reporting; native fallback for unaudited titles;
  1080p/60 at 3x.

### Slice 4 - 2D sink phase 2a

- CPU per-line BG/OBJ decode at the widened width, GPU layer selection,
  windows, blending, 3D insertion; top-screen output at `width x 192`.
- Keep the CPU compositor as the reference/fallback.
- **Gate:** 2D bit-exact where the CPU path is bit-exact; 3D tolerance holds;
  1080p/60 at 3x 3D + 2x-3x 2D; 1440p/60 at 4x 3D + 3x 2D (M8 gate);
  guest RAM/save/frame-digest identity; capture works.

### Slice 5 - alpha toggle, verification, reporting

- One master toggle in the TUI, default off; advanced flags; verify mode;
  startup failure text; fallback counters and first-occurrence report.
- **Gate:** default-off run is unchanged; enabling implies native presentation;
  wrong pixels are never silently presented; docs and `Plan.md` updated; the
  alpha is clearly not M6/M8 completion.

### Optional later

- 2b full GPU 2D decode, if profiling shows the CPU decode is the bottleneck.
- 32:9 (682) after the 448 caps are raised.
- Post-FX and frame decoupling remain their own M8 decisions.

## 10. Verification

### 10.1 Guest state (hard gate)

Same input replay, native vs CPU:

- guest RAM hash identical;
- save bytes identical (golden-file round-trip);
- guest-visible frame digest identical, including capture frames;
- scheduler/frame counters identical.

The host presented-frame FNV is a same-renderer determinism check. It is
expected to differ across renderers when the 3D sink is within the permitted
1-LSB tolerance; it is not used to excuse a guest-state difference. Capture and
verify frames force the CPU path, so guest-visible VRAM is identical.

### 10.2 3D

- Native scale: per-channel color difference at most 1 LSB; coverage/attr lines
  exact; any larger difference or any geometry mismatch fails.
- Verify mode renders CPU and sink for the same frame and dumps a diff on
  failure. The CPU readback stays available.
- Textures, polygon IDs, and blend inputs are compared, not just final color.

### 10.3 2D

- Bit-exact where the CPU path is bit-exact (integer math, same inputs).
- Where the path is not bit-exact, compare against the CPU reference with a
  documented tolerance; do not silently widen the tolerance.
- Golden frames are generated locally from the user's ROM; nothing derived from
  ROM content is committed.

### 10.4 Performance

- GPU timestamps around the 3D sink, 2D sink and presenter; frame-time p50/p99.
- Gates from `Plan.md` M8: 1080p/60 at 3x 3D + 2x-3x 2D first; 1440p/60 at 4x
  3D + 3x 2D as the M8 gate; 4K is a stretch target.
- Logic stays locked at 60 Hz; presentation may be paced separately later, but
  this spike does not change the clock.

### 10.5 Failure injection

- Missing/unsupported Vulkan device, shader compile failure, pipeline failure,
  out-of-memory, device loss, unsupported texture format, missing policy file.
- Startup failures are fatal and loud. Per-frame failures fall back to CPU,
  report once, and increment a counter. A repeated failure disables the sink
  and tells the user to relaunch; it does not keep presenting suspect pixels.

## 11. Patch and build plan

`patches/ndsrecomp/0003-native-render-sink.patch` (guarded by
`PKMNG5PC_NATIVE_RENDERER`, so standalone ndsrecomp builds are unaffected):

1. `frontend.cpp`: call `pkmng5pc_ds_render_init()` after
   `pkmng5pc_native_present_init()` and after `nds_gpu3d_set_output_width()` so
   the sink validates the effective width at install; call shutdown on
   teardown; include the native renderer in both `nds_gpu2d_set_hd_emit`
   conditions.
2. `patches/ndsrecomp/0004-gpu3d-device-accessor.patch`: expose the caller's
   `melonDS::GPU3D*` so the bridge installs the sink through
   `SetCurrentRenderer`; keep `SetCurrentRenderer` as the primitive.
3. `frontend_config.cpp`: accept any even 256..448 for `adaptive_width` and
   update the error text.
4. `main.cpp`: accept the same range for `NDS_WIDESCREEN_WIDTH` and
   `--widescreen-width`; mark the native path as `adaptive_supported |= top`
   when `PKMNG5PC_WIDESCREEN_SUPPORTED` asks for it, before the fail-closed
   check.

In the no-macro branch, a set `PKMNG5PC_RENDERER` still prints the loud error
and exits, so a stale TUI setting never silently falls back.

`cmake/Runner.cmake` applies the patches and links `pkmn_render_ds`.
`cmake/Shaders.cmake` pins `glslang`, compiles every shader, and generates the
embedded C++ arrays. `NOTICE` gains the glslang BSD-3 entry when the dependency
lands. `AGENTS.md` records the fourth patch and the build-option note at the
same time.

## 12. Open items

- Black scene audit: which menus/battle screens opt out; where the HUD anchor
  must differ from the existing adaptive logic.
- Exact guest-visible frame-digest harness and where it hooks the existing
  digest ring.
- 342 width audit in the compute tile/sky/HD paths; 32:9 cap sites.
- 2b decision after profiling.
- Post-FX integration point and frame decoupling (M8, separate decisions).
- D3D12 backend and web/Android constraints (recorded, not scheduled).
