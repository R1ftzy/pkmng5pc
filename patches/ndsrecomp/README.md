# patches/ndsrecomp/ — local modifications to the pinned submodule

`third_party/ndsrecomp` is a pinned git submodule (tag `v0.6.10`) that this
project does not own. These `*.patch` files are the only modifications we make
to it, and they are **applied at configure time** by
`cmake/Runner.cmake::pkmng5pc_apply_ndsrecomp_patches()` — idempotently, only
when `PKMNG5PC_BUILD_RUNNER=ON`.

Applying a patch leaves the submodule checkout dirty (`git status` in the
superproject reports `modified content`). That is intended and reversible:

```sh
git -C third_party/ndsrecomp status          # what is applied
git -C third_party/ndsrecomp checkout .      # revert, then re-configure
```

Configure detects a reverted submodule (`git apply --check` succeeds) and
re-applies; it detects an already-applied one (`git apply --check --reverse`
succeeds) and leaves it alone. If neither direction applies, configure fails
with the restore command rather than guessing, because that means the submodule
moved or carries unrelated edits.

This is the same convention ndsrecomp itself uses for its vendored melonDS tree
(`third_party/ndsrecomp/runner/vendor/melonds/patches/README.md`): the patch
files stay in-tree as the GPLv3 §5(a) record of what changed, and the change is
kept as small as the seam allows.

Every hook added here must be **compiled out** unless the port's build defines
its guard macro, so the patched tree still builds standalone as plain
ndsrecomp. `nds_runner` gets the guard macro only from `cmake/Runner.cmake`.

## 0001-native-present-hook.patch

`runner/src/frontend.cpp`: lets `tools/play -Native` present the two DS screens
through this project's own presenter (`render/`) instead of the frontend's
fixed 512×768 SDL blit. Recorded in
`docs/adr/0006-ndsrecomp-native-present-hook.md`.

Six small sites, all behind `#if defined(PKMNG5PC_NATIVE_PRESENT)`:

1. Forward declarations of the bridge (`render/src/native_present.cpp`).
2. `create_presentation()`: force the stacked single-window layout, create that
   window with `SDL_WINDOW_VULKAN`, and skip the SDL renderer/texture setup —
   the presenter owns those. Window ids are still registered so the existing
   input and touch plumbing is untouched.
3. `convert_mouse_event_to_logical_coordinates()`: ask the presenter for the
   window-to-logical conversion instead of `SDL_RenderCoordinatesFromWindow`,
   which needs the renderer this path does not create. The presenter inverts the
   same placement rule it composed with, so the touchscreen is clickable exactly
   where it is drawn.
4. `present_screens()`: forward the raw surfaces to the bridge and return,
   before any SDL work.
5. `nds_run_interactive_frontend()`: bring the presenter up after the window
   exists, and fail loudly if it cannot.
6. The same function's teardown: shut the presenter down before the window is
   destroyed.

Nothing above changes guest execution, input, audio, pacing or the
presented-frame digest ring: it is presentation, plus the pointer-coordinate
conversion the replaced renderer used to perform.
