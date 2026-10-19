# The frontend hook: the runner presents through our seam

**Status:** accepted (2026-10-19). Partly supersedes
`docs/adr/0005-vulkan-first-renderer.md` ("the runner is a pinned pristine
submodule with no present hook").

`Plan.md` M8 wants the two DS screens laid out and post-processed by our own
present/compositor, replacing the runner's fixed 512×768 top-over-bottom blit.
ADR 0005 fixed the seam (`render/backend.h`, `pkmn::render::Presenter`) and made
Vulkan the first backend, and deliberately left the wiring open.

Reconnaissance for this ADR found there is **exactly one** present site —
`present_screens()` in `third_party/ndsrecomp/runner/src/frontend.cpp` — with no
callback, no environment switch and no library target: `nds_runner` is one
executable and that function is file-local. So the seam cannot be reached
without either adding a call site inside the vendored frontend, or replacing the
frontend outright.

## Decision

Add a small, opt-in call site to the pinned submodule, carried as a **tracked
patch** (`patches/ndsrecomp/0001-native-present-hook.patch`, applied at
configure time by `cmake/Runner.cmake`). The port's own bridge
(`render/src/native_present.cpp`) is linked into `nds_runner` and does the
presenting. Selection is by `PKMNG5PC_PRESENT=native`, which `tools/play
-Native` sets.

The hook is presentation only. The frontend keeps the window, input, touch
mapping, controllers, audio queue and pacing, and keeps producing the
presented-frame digest ring — so a `-Native` run and a plain run are the same
guest run, and the digest still proves it. The one thing the hook does supply is
the renderer's usual window-to-logical pointer conversion, because this path
creates no SDL renderer to ask (see "Recorded details").

The whole hook sits inside `#if defined(PKMNG5PC_NATIVE_PRESENT)`, and that
macro is defined only by this project's build. A patched tree therefore still
builds and behaves as plain ndsrecomp. It is not a conditional feature of ours;
it is a build-time-absent one.

## Why not the alternatives

- **A second window owned by the bridge, submodule untouched** — the smallest
  patch (one site) but two visible windows and broken focus/pointer routing.
  Choosing the honest integration also keeps the eventual M8 work (layout,
  post-FX, frame decoupling) attached to the one window the game already uses.
- **Our own frontend loop** — the literal reading of M8's "decoupled from the
  runner's frontend", and the eventual destination for frame decoupling and the
  layout policy. It is not a first step: it means re-implementing input, touch,
  controllers, the audio device and queue, savestate slots, the diagnostics
  bridge and the debug server before a pixel appears, and it needs the
  emulation exposed as a library (another submodule change). Rejected *for now*
  — this ADR is the incremental step that makes the seam real, not the last one.
- **Fork ndsrecomp** — moves the same diff into a repository we must then keep
  merged against upstream. Rejected: the diff is six small sites, the guard
  macro keeps it inert for everyone else, and `patches/` is already this
  project's convention for pinned third-party sources, mirrored from
  ndsrecomp's own `runner/vendor/melonds/patches/`.

The cost accepted: `git status` in the superproject reports the submodule as
having modified content, and a submodule bump needs the patch re-based. Both are
visible and reversible (`git -C third_party/ndsrecomp checkout .`), and
configure refuses to guess if the patch applies in neither direction.

## Recorded details

- **The surfaces are already BGRA8.** `nds_gpu2d_framebuffer()` is ARGB8888
  (B,G,R,A in memory) and the frontend uploads it with
  `SDL_PIXELFORMAT_ARGB8888`, so the bridge copies into the presenter's canvas
  with no conversion. ADR 0005 describes `compose_screens()` as taking 256×192
  **BGR555**; that is still true of the smoke tool's input, but the runner never
  produces BGR555, so composition over BGRA8 lives beside it as
  `compose_screens_bgra8()`. Both share the placement rule and the letterbox, so
  layout still has one home.
- **The canvas is built at the window's pixel size.** The Vulkan presenter
  requires canvas extent == swapchain extent, so the bridge resizes the
  presenter from `SDL_GetWindowSizeInPixels()` and composes at that size; the
  integer scale and letterbox are `compose_screens_bgra8()`'s business.
- **The layout rule has exactly one home, including its inverse.** The
  compositor draws from `stack_placement()`/`place_screens()` and the pointer
  mapping reads the same function, so a click cannot drift from the pixels it
  is aimed at. `stack_placement()` is public because the frontend needs the
  inverse; `ScreenPlacement` stays internal.
- **`-Native` is stacked only.** The presenter owns one output surface, so the
  hook forces the stacked layout and `tools/play` warns if `-Layout separate`
  was asked for.
- **Adaptive wide top works.** The bridge passes the real widths through, and
  `compose_screens_bgra8()` gives each screen its own width at the shared
  integer scale, so a widened top screen does not stretch the bottom one.
- **Pointer/touch is mapped by the compositor.** The frontend normally converts
  pointer positions through the SDL renderer's logical presentation, and this
  path has no renderer. `convert_mouse_event_to_logical_coordinates()` therefore
  asks the presenter instead (`pkmng5pc_native_present_logical_point`), which
  inverts the *same* placement rule the frame was composed with
  (`stack_placement()` in `render/src/compose.cpp`) and converts window points
  to pixels for high-DPI. Clicks on the top screen or the letterbox map outside
  the bottom screen and are rejected by the frontend's existing routing, so a
  click can never touch through the wrong screen. Verified with the frontend's
  own menu self-test (`NDS_FRONTEND_SELFTEST_MENU=1`): `-Native` now reports
  `touch_presses=1 last_touch=(127,372)` - byte-identical to the SDL path, which
  reported `0`/`(-1,-1)` before the fix - and a direct guest read agrees on both
  paths (EXTKEYIN `0x7F` idle -> `0x3F` pen-down on the bottom screen; no
  pen-down for a top-screen click).
- **Failure is loud.** If Vulkan cannot come up, `-Native` reports it and exits
  rather than presenting nothing. There is no silent fall back to the SDL blit,
  because the window was created for Vulkan and half-configured presentation is
  worse than a clear error.

Considered and not chosen here: passing `--present native` on the runner command
line instead of an environment variable. The frontend's option parser is a
second patched file, for a switch the environment already carries cleanly, the
same way `NDS_SDL_RENDER_DRIVER` and the other frontend knobs do.
