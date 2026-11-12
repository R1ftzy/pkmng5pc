# wide output (top-screen band)

## Target

- ROM file: the Black dump supplied at run time (`roms/<your-rom>.nds`).
  Deliberately not recorded here: no dump filename, release name or hash is kept
  in the repo (`AGENTS.md` §2.2).
- Region / revision: not recorded, same reason.
- Game code: `IRBO` (Black), the title used for the capture.

## Tools

- local render path, `render/src/native_present.cpp` and
  `render/src/compose.cpp` (source of the rules being measured)
- runner build `windows-relwithdebinfo` with `PKMNG5PC_NATIVE_PRESENT=ON`,
  `PKMNG5PC_BUILD_RUNNER=ON`, `PKMNG5PC_BUILD_RENDER=ON`,
  `NDS_GPU2D_THREADED=0` (inline scanline path)
- Python 3.14.3, numpy 2.5.0, Pillow 12.2.0 for the image comparison
- capture harness: `banks/wideprobe.py` (digest ring) and `banks/widecap.py`
  (frame writer), both git-ignored scratch

## Observations

- **W1.** An interactive run with `NDS_ADAPTIVE_WIDESCREEN=top`,
  `NDS_WIDESCREEN_WIDTH=342` presents `top=342 bottom=256` on 413/413 captured
  frames. A 4:3 run presents `256/256` on 410/410. A Compatibility-mode run with
  the wide environment set still presents `256/256` on 405/405, so the SDL path
  ignores it.
- **W2.** Save-loaded overworld run, 4:3: top screen captured at 256x192
  (`banks/shots-wide/base/t0060.02s-A.png`). Same route and timing at 342 wide:
  presenter swapchain 684x768, top band 342x192, runner log
  `[sdl] adaptive=top` (`banks/shots-wide/wide/t0060.01s-A.png`). Both captures
  are at emulated t~60 s, on the bridge after CONTINUE.
- **W3.** Comparing the 342-wide capture against the 4:3 capture:

  | Candidate | Mean abs diff | p95 | max |
  | --- | --- | --- | --- |
  | 342 centre crop (columns 43..298) vs 4:3 | 1.24 | 4.0 | 37 |
  | 342 downscaled to 256 vs 4:3 | 11.14 | 57.0 | 148 |

- **W4.** The player sprite's dark cluster near the screen centre occupies
  x 127..130, y 90..95 in the 256 capture and x 170..173, y 90..95 in the 342
  capture: same 4x6 px footprint and the same position relative to the 342
  centre (x 171).
- **W5.** `third_party/ndsrecomp/runner/src/gpu3d.cpp` (`GPU3D::SubmitVertex`)
  rescales clip X by `256 / RenderWidth` when the guest viewport is 0..256, while
  the 2D band is composed at the full adaptive width.
- **W6.** Presented-frame digests are identical between the 256 and 342 runs for
  the frames both captured (guarded by the digest ring); changing the presented
  coverage does not change guest execution.

## Inferences

- (confidence: high) The 342 path is **true widening**, not a stretch: the
  centre 256 columns survive unchanged (W3), the sprite keeps its size and
  centre (W4), and the extra band carries scene content the guest already
  submits (W5). A 342->256 rescale would have produced the much larger
  differences in W3's second row.
- (confidence: high) Wide output is therefore an output-path result that needs
  no guest-side frustum change for this scene; the guest's own frame digests do
  not move (W6).
- (confidence: medium) The side band can still be limited by guest-side culling
  or streaming that only populates the 0..256 view volume. Not observed as
  visibly wrong on the tested route (bridge, NPCs, terrain all extend into the
  band), but only one scene was measured.

## Unknowns

- Whether other scenes (battle, menus, indoor maps, cutscenes) keep the band
  filled or show culling artefacts at 342/448.
- The safe-area policy: which HUD/UI elements must stay inside 4:3 when the band
  widens. Not measured; the design still calls for a per-title policy.
- The 448 (21:9) band was exercised by the digest ring but not compared
  frame-for-frame against 4:3 the way 342 was.
- Whether a guest wide-frustum patch is ever needed. It is not needed for the
  overworld scene above; it would only matter if culling/streaming is shown to
  leave the band visibly empty.

## Cross-ROM comparison

Not performed. The render path is title-independent; a per-title visual audit
of the wide band (Black first, then White, B2, W2) is the follow-up.
