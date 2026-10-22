# Native DS render sinks: Vulkan 3D first, then 2D

**Status:** accepted (2026-10-21) as an owner-approved early M8 spike. Default
off. This does not mark M6 or M8 complete, and it does not authorize M8's
post-FX, frame-decoupling or interpolation work. The detailed design and slice
plan live in `docs/design/native-render-sinks.md`.

The default path still rasterizes and composites the DS GPU on the CPU and uses
the host GPU only for presentation (`GLOSSARY.md`: *DS pipeline*). The 3D soft
rasterizer and the 2D scanline compositor are now the remaining per-frame
graphics cost, and they are also the reason the DS's 256-pixel output cannot be
widened or rendered above native resolution. M8 needs both, but M8 is gated on a
finished M6; the owner explicitly authorized this spike ahead of that gate.

The pieces already exist and are being reused, not reinvented:

- `NdsGpu2dHdFrame` (`third_party/ndsrecomp/runner/src/gpu2d.h`) describes the
  resolved 2D stack with the 3D layer removed: top and below RGB6 pairs, width,
  3D priority/order, BLDCNT, master brightness, EVA/B/Y and `render_xpos`.
- The OpenGL compute renderer's `ComputeHost.cpp` already consumes that frame
  and re-inserts the hi-res 3D surface with the DS's own blend rules. It is the
  algorithm to port, not a new compositor design.
- The Vulkan presenter (`render/src/vulkan_presenter.cpp`) already owns a
  device, a swapchain and the native window hook from ADR 0006. It is
  deliberately shader-free today (ADR 0005).

The guest GPU device model stays authoritative throughout: VRAM, OAM,
palettes, display capture, savestates, the CPU reference path and the
presented-frame readback remain exactly where they are. The sinks replace
rasterization endpoints only.

## Decision

1. **Two sinks.** The *3D sink* implements melonDS's `Renderer3D` and replaces
   only the GE rasterizer; geometry decode stays in the vendored device model.
   The *2D sink* replaces the 2D scanline compositor and produces the final
   screen image from latched per-line layer state; the alpha starts with the top
   screen and leaves the bottom native until its policy is audited. The 3D sink
   lands first; the 2D sink lands second.
2. **One shared host context.** A private `VulkanContext` under `render/src/`
   owns the instance, physical/logical device, queue and allocator.
   `VulkanPresenter` remains the sole swapchain owner and gains an internal
   DS-composite hook. `render/backend.h` does not change. Emulation and
   presentation already run on the frontend thread, so one queue needs no
   cross-thread synchronization; the sinks never issue Vulkan work from the
   band-pool workers.
3. **New target, tracked install.** The sinks live in `render/src/ds/`
   (`pkmn_render_ds`), linked into `nds_runner`. A tracked patch
   (`patches/ndsrecomp/0003-native-render-sink.patch`, applied after `0001`)
   installs the renderer through `GPU3D::SetCurrentRenderer` after
   `pkmng5pc_native_present_init()` and enables HD emission when the native
   path is active. The patch compiles out without the port's build macro, like
   patch `0001`.
4. **`Accelerated=false`.** The 3D sink reports the non-accelerated contract and
   keeps the existing `GetLine` / `wide_line` behaviour, so the CPU fallback,
   display capture and digest paths keep working. No GL-compute-specific bridge
   is reused. The separate `NdsGpu3dRendererPolicy::Vulkan` diagnostic value
   proposed during review was dropped: `gpu3d.h` keeps auto/soft/compute, and
   the sink reports through the bridge's stderr notices instead.
5. **First visible slice is the HD split.** Vulkan renders the 3D surface at
   low and high resolution; the CPU adaptive compositor still emits
   `NdsGpu2dHdFrame`; the Vulkan presenter composites 2D top/below plus the
   hi-res 3D surface and scales to the window. The 2D sink is not a prerequisite
   for the first 3D win.
6. **Port the existing compute renderer to Vulkan compute.** Keep its
   low-res/hi-res split, tile scheme and texture cache; replace the GL 4.3
   compute shaders, SSBOs and GL texture loader with Vulkan GLSL 450, explicit
   descriptor sets, push constants and a Vulkan texture-cache loader. The
   low-res color/attribute readback still feeds the CPU fallback, capture and
   digest.
7. **Shaders are built, not shipped as blobs.** GLSL is compiled to SPIR-V at
   build time by a pinned `glslang` FetchContent dependency, then embedded into
   the binary as generated C++ arrays. No runtime shader compiler, no committed
   SPIR-V, and a BSD-3 `NOTICE` entry when the dependency lands. The build
   option defaults OFF; enabling it is explicit, and a configure failure is
   loud. The default (renderer-off) build must remain usable.
8. **2D sink phase 2a.** Keep the CPU tile/OBJ decode, extend it to the widened
   range (`x_native = x - extra`), and move the per-pixel layer selection,
   priority/order, window masks, blending and 3D insertion to the GPU. Full GPU
   tile decode (2b) is optional and only after profiling proves it pays.
9. **Widescreen is true scene widening.** The 3D frustum widens through the
   existing guest-projection path; 2D backgrounds sample beyond the DS's
   256-pixel clip; authored UI stays inside a 4:3 safe area. The policy is
   per-title and per-screen, Black first, with a graceful native-width
   fallback. Stretching is not widescreen; a title without a wide policy falls
   back to native width and is letterboxed/pillarboxed as needed.
10. **Aspect ratios are selectable in the TUI.** 4:3 = 256, 16:9 = 342 and
    21:9 = 448 source pixels, each 192 rows; 32:9 = 682 is recorded but deferred
    until the 448 cap is raised. The selected ratio sets the source width; the
    presenter letterboxes or pillarboxes to the window. The per-title policy
    decides which screens receive the width.
11. **One alpha master toggle, default off.** It implies native presentation.
    Advanced per-sink switches (`PKMNG5PC_RENDERER=3d|2d|both`) exist only for
    debugging. Startup failure is fatal with a clear message; a per-frame sink
    failure falls back to the CPU path for that frame and is reported once and
    in the stats. Enabling the mode never silently presents stretched or wrong
    pixels.
12. **Verification is part of the feature.** 2D is bit-exact where the path is
    bit-exact; 3D is at most 1 LSB from the CPU renderer at native scale. The
    hard cross-renderer gate is guest state: RAM, save and the guest-visible
    frame digest must match. The host presented-frame FNV is a same-renderer
    determinism check, because a permitted 1-LSB 3D difference changes it;
    capture and verify frames force the CPU path so guest-visible VRAM is
    identical. Dual-render verify mode and the CPU readback are retained.

## Consequences

Positive: high-resolution 3D and true widescreen become possible without
touching game logic; the 3D win arrives before the larger 2D sink; the guest
device model, savestates and capture stay authoritative; the CPU path remains
the reference and the fallback; the presenter seam from ADR 0005/0006 is
reused.

Costs and risks: a context refactor and a new compute backend; a build-time
shader toolchain and two more tracked patches (`0003`, `0004`); a per-frame
low-res readback stall;
GPU memory for low/high-res surfaces and the texture cache; a 448-pixel source
cap; a per-title widescreen audit; Vulkan/GLSL fidelity risk in the compute
port; and driver variance. The two sinks do not make M6 or M8 complete, and the
alpha must not be described as such.

## Recorded implementation facts

- `NDS_HAVE_COMPUTE_RENDERER` is off in this build. `Accelerated` is therefore
  overloaded, and `Accelerated=true` would route through GL-compute arrays that
  are compiled out. `Accelerated=false` routes `nds_gpu3d_line` /
  `nds_gpu3d_wide_line` through `GPU3D::GetLine` and permits width changes.
- There is no `nds_gpu3d_set_renderer`; the install primitive is
  `GPU3D::SetCurrentRenderer`. `ScrolledLine` and `ScrolledAttrLine` are
  `[448]`, and the 2D adaptive buffers cap at `kMaxAdaptiveWidth = 448`.
- `nds_gpu3d_set_output_width` already accepts any even width from 256 to 448.
  `nds_parse_widescreen_width` and `display.adaptive_width` are the gate: they
  accept only 256/320/384/448 today. The patch must widen them to the even
  256..448 range so 16:9's 342 is legal.
- Gen 5 has no `adaptive_supported` capability set (only the SM64DS path sets
  it), so the patch adds a `PKMNG5PC_WIDESCREEN_SUPPORTED` override before
  `main.cpp`'s fail-closed check; the TUI sets it to `top` for the alpha. The
  per-title policy still decides the actual width; a title without a policy
  stays at 256.
- `nds_gpu2d_set_hd_emit` is gated on `initial_internal_resolution > 1 &&
  presentation.gl_top` at `frontend.cpp:2004` and in the governor at
  `frontend.cpp:1767`. Both sites need the native renderer in the condition.
- Display capture (`nds_gpu2d_set_direct_present`, capture frames) forces the
  CPU 2D path. Capture and savestate load must flush the sink and invalidate
  the HD frame before the next frame.
- The TUI's native-toggle wording must distinguish *native presentation*
  (`render/`, Vulkan blit) from *native rendering* (the sinks). They are
  different features and the alpha adds the second.

## Considered and rejected

- **2D sink first** - rejected: the 3D rasterizer is the larger cost and the HD
  split already delivers a visible 3D win without it.
- **Present the CPU framebuffer through the Vulkan blit only** - rejected: it
  adds no rasterization throughput and cannot produce wide 3D.
- **Use `Accelerated=true` and reuse the GL compute bridge** - rejected: the
  build has no GL compute renderer, and the contract is tied to GL objects.
- **Separate Vulkan devices for presenter and sink** - rejected: copies and
  cross-device synchronization for no benefit on one thread.
- **Full GPU 2D tile decode immediately** - rejected: the CPU decode is cheap
  compared with the 3D work; phase 2a gets the compositing win first.
- **Hardcode per-title support by dump SHA-1** - rejected: the title policy is
  data, and dump identity must not be baked into rendering code.
- **Stretch the native 256-pixel image to a wide window** - rejected: that is
  not scene widening and changes the authored framing.
- **System `glslc` / committed SPIR-V blobs** - rejected: the build must be
  reproducible on both hosts and the repo ships no generated shader binaries.
