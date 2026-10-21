# The renderer seam is ours; Vulkan is the first backend

**Status:** accepted (2026-10-19); partly superseded by
`docs/adr/0006-ndsrecomp-native-present-hook.md`, which wires the seam into the
runner's frontend through a tracked, guarded patch and records why the
"pinned pristine submodule" premise below does not survive that step.

`Plan.md` M8 needs our own present/compositor: the runner's frontend currently
owns the window, the layout and the present, so there is no place to put
widescreen layout, post-FX, or a resolution policy that does not touch game
logic. `AGENTS.md` §5 puts a single seam between the port and the GPU
(`render/backend.h`) with Vulkan, D3D12 and OpenGL as possible backends.

This ADR fixes two things: **where the seam is**, and **which backend is first**.

## The seam

`pkmn::render::Presenter` owns a device and one output surface. The port hands it
a finished BGRA8 canvas and a `PresentTarget` rectangle in output pixels; the
presenter scales and letterboxes on the GPU and puts pixels on the screen.
Nothing above `render/backend.h` names Vulkan/D3D/GL, and nothing below it names
the game — the same rule the HAL already follows (`AGENTS.md` §5).

`create_presenter()` never returns null and never throws on a missing driver: an
unavailable backend degrades to a **Null presenter**, and `init()` reports
whether a real device came up. That keeps every consumer (the smoke tool today,
the launcher/frontend later) free of backend conditionals.

Layout lives in exactly one place, `compose_screens()`: two 256×192 BGR555
screens in, one canvas of the requested output size out, stacked or side-by-side
with a letterbox. This is M8's "one home for layout"; aspect-ratio handling and
later post-FX attach to the same entry point rather than to the frontend.

## Vulkan first

Vulkan is the first backend because it is the one that exists on both current
host targets with the feature set M8 names (compute for post-FX, explicit
resolution control, no implicit driver-side state). D3D12 and OpenGL 4.5 stay
recorded in §5 as later/fallback backends; the seam already admits them, and the
build produces the Null backend when Vulkan is switched off.

## The backend is deliberately shader-free to start

The Vulkan presenter uploads the canvas through a staging buffer, copies it into
a device-local image (`vkCmdCopyBufferToImage`), and scales it into the swapchain
with a nearest-neighbour `vkCmdBlitImage`. There is no pipeline, no descriptor
set and no shader module — and therefore no `glslc`/`glslangValidator` build
dependency. The point is to exercise the whole seam (device selection, surface,
swapchain, resize, out-of-date/recreate, present, validation layer when
installed) against the DS's real CPU-side 2D/3D output before any GPU-side
compositing exists. The blit is the first GPU-side stage; shader-based post-FX
replaces it when the compositor lands, without moving the seam.

Consequences of that choice, recorded so they are not mistaken for oversights:

- No SDK and no import library are required to build. `VK_NO_PROTOTYPES` is
  defined and every entry point is resolved at run time through
  `vkGetInstanceProcAddr`/`vkGetDeviceProcAddr` (`render/src/vulkan_functions.*`);
  headers come from `KhronosGroup/Vulkan-Headers` via `FetchContent` unless a
  local SDK is found (`cmake/HostDeps.cmake`). The loader is the system one —
  SDL3 supplies it.
- There is no image-sampling path yet, so no sampler/filtering policy is decided
  by this ADR. Nearest-neighbour at native scale is the assumption to confirm
  when the compositor lands.
- GPU-side compositing, 3D resolution scaling and post-FX are **still open** and
  get their own decisions (`Plan.md` M8: "a new ADR is owed" for both the frame
  decoupling and the resolution policy).

Verified: `pkmn_render_smoke --backend null --frames 3` and `--backend vulkan
--frames 10` (real swapchain on Intel Arc, `presented=10 size=512x768`),
`--width/--height` driving a real swapchain recreate (`size=1268x1489`), and
`--validation` printing `validation requested but unavailable; continuing` with
the layer absent.

Considered:

- **Renderer inside the runner's frontend** — rejected: the runner is a pinned
  pristine submodule and its present path is fixed 512×768 top-over-bottom with
  no layout or post-FX hook.
- **bgfx/sokol as the abstraction** — not rejected on merit, deferred: with one
  backend and a shader-free presenter they would add a dependency without
  removing work, and `AGENTS.md` §4 asks for a dependency check before adding
  one. Revisit when a second backend is actually built.
- **OpenGL first** — rejected: it is recorded as the fallback/debug backend, and
  the near-term work (compute post-FX, explicit resolution) is where GL is
  weakest on Windows.
- **Shipping a compiled shader blob now** — rejected: it would couple the build
  to a shader compiler toolchain before there is a shader.
