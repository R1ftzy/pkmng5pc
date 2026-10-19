#pragma once

// Bridge between the runner's interactive frontend and this library's
// `Presenter` seam (`docs/adr/0006-ndsrecomp-native-present-hook.md`).
//
// The frontend owns the window, input, audio and pacing; this bridge only takes
// over *presentation*. It is compiled into `nds_runner` by `cmake/Runner.cmake`
// and reached from a small, opt-in hook in
// `third_party/ndsrecomp/runner/src/frontend.cpp` (carried as
// `patches/ndsrecomp/0001-native-present-hook.patch`). The hook is compiled out
// of builds that do not define `PKMNG5PC_NATIVE_PRESENT`, so the pinned
// submodule still builds standalone.
//
// The ABI is plain C with no SDL or Vulkan types, because the patched frontend
// declares these by hand rather than including this header.
//
// Selection is by environment (`PKMNG5PC_PRESENT=native`, set by `tools/play
// -Native`) so the frontend needs no new command-line surface.
//
// Surfaces are handed over exactly as the frontend already has them: ARGB8888
// (BGRA8 in memory) rows of `height` 192, `width` 256 or the adaptive wide
// width. The guest never sees any of this.

extern "C" {

// 1 when the environment selects the native presenter. Cheap and cached; the
// frontend calls it every frame.
int pkmng5pc_native_present_enabled(void);

// Bring up the presenter on an SDL window created with SDL_WINDOW_VULKAN.
// Returns 1 on success, 0 on failure (the caller reports it and must not
// present through the bridge).
int pkmng5pc_native_present_init(void* sdl_window);

// Compose and present one frame. Returns 1 when the frame is handled (including
// a skipped frame after a recoverable swapchain error) and 0 if no presenter is
// live. Never returns a fatal failure for a resize: it re-derives the swapchain
// from the window.
int pkmng5pc_native_present_frame(
    const unsigned int* top, int top_width, const unsigned int* bottom, int bottom_width);

// Release the device. Safe to call more than once, and safe when no presenter
// was ever brought up.
void pkmng5pc_native_present_shutdown(void);
}
