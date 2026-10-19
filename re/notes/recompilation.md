# recompilation / static-recompilation tooling (D7)

## Context

Plan §2 resolves D1 as **static recompilation (B)**: recompile the games' ARM9 +
overlays + ARM7 to C++, implement the hardware as a HAL, ship no emulator core.
D7 asks *which* recompiler to use. Constraint: this project is
**GPL-3.0-or-later** (see Decision), so GPL-derived components (melonDS trees) are
compatible; proprietary/GPL-incompatible code still may not be vendored
(`AGENTS.md` §2.7/§4).

## Method families (settled 2026-10-14)

There are two ways a "PC port" of a console game gets made. We are squarely the
**static-recompilation family**; the other family needs source we do not have.

| Project | Method | Game code source | Hardware seam |
| --- | --- | --- | --- |
| **Sonic Unleashed Recompiled** | **Static recompilation** (XenonRecomp: PPC→C++; XenosRecomp: Xenos shaders→HLSL) | generated C++ from the retail binary | full **runtime** (mem mgmt, FS, sync, audio, video output) |
| **Zelda 64: Recompiled** (Majora's Mask; OoT planned) | **Static recompilation** (`N64Recomp`, MIPS→C) | generated C from the ROM | full hardware **runtime** |
| **ndsrecomp** (this project's recompiler + runner) | **Static recompilation** (ARMv4T/v5TE→C++) | generated C++ from `arm9.bin`/overlays/ARM7 | dual-CPU runtime + HAL |
| Ship of Harkinian (the well-known OoT PC port) | **Decompilation port** (OoT decomp + `libultraship`) | **human-written C source** (the decomp) | thin platform layer |

Naming trap: the famous OoT PC port (**Ship of Harkinian**) is *not* a recomp —
it is a **decompilation port**, possible only because a full matching
decompilation of OoT exists. The genuine "Zelda recomp" is **Zelda 64:
Recompiled** (`N64Recomp`), which is the same family as Sonic Unleashed
Recompiled and `ndsrecomp`.

Recomp projects never ship an emulator core: the CPU's machine code is
translated ahead of time and the result runs natively. What remains is a
**runtime** that services the hardware the translated code pokes through
memory-mapped registers ("memory management, file system, synchronization,
audio, and, most importantly, video output" — Unleashed Recompiled's lead).
That runtime is unavoidable in *every* recomp port; it is not an emulator
wrapper.

**Decision: option 1 — the faithful floor (recomp family).** We are
Sonic-Unleashed-Recompiled / Zelda64Recomp, *not* Ship of Harkinian, because
**no Gen 5 decompilation exists** to port. A Ship-of-Harkinian-style approach
for Gen 5 is only reachable later by hand-writing native replacements over the
recompiled code — i.e. the HLE layer.

## Candidate tools (surveyed 2026-10-14)

| Tool | What it is | License | Verdict |
| --- | --- | --- | --- |
| `RetroPortingToolKit/ndsrecomp` (ex `mstan/ndsrecomp`) @ **`v0.6.10`** | DS static recompiler + runtime; ARM9/ARM7 dual-CPU scheduler; HLE layers | **MIT** (repo own code, this tag) | **Adopt** (pinned tag; see below). |
| `RetroPortingToolKit/ndsrecomp` @ `main` | newer (GameCube-/Mario-64-style work, `PPC`?) | **PolyForm Noncommercial 1.0.0** + GPL-3.0 combined | **Do not use.** |
| `mstan/arm-recomp-core` (submodule `external/arm-recomp-core`) | the recompiler core | **MIT** | Usable |
| `mstan/freebios` (submodule `third_party/freebios`) | DraStic BIOS replacement | **BSD-2-Clause** | Usable (D8) |
| `Myth7x/ndsrecomp` | ROM → standalone C/CMake project (Python generator) | **No license file** | **Unusable.** Black/B2W2 listed as not booting. |
| `AetiasHax/ds-decomp` (`dsd`) | decomp toolkit: extract, delink, `dis`, `objdiff`, symbols | **MIT** | Usable (RE tooling) |

### License boundary inside ndsrecomp (v0.6.10)

- **MIT** — repo's own code: `recompiler/`, `runner/` (core), `tools/`,
  `launcher/`, `bios/`, docs, plus the `arm-recomp-core` submodule.
- **BSD-2-Clause** — `third_party/freebios`.
- **GPL-3.0-or-later** — everything derived from **melonDS**:
  `runner/vendor/melonds*`, `runner/src/melonds_compute` (accelerated 3D), and
  the `oracle/` differential harness.

Correction (2026-10-14, after reading the tree): the GPL scope is **larger**
than the first survey suggested. `THIRD_PARTY_ATTRIBUTION.md` is explicit:

- GPL-3.0-or-later = **all of** `runner/vendor/melonds/` (the melonDS **3D
  geometry engine, both the software rasterizer `GPU3D_Soft` and the OpenGL
  compute renderer**), `runner/src/melonds_compute/`, the Wi-Fi device model +
  net glue (`Wifi`/`WifiAP`/`net/`), `runner/src/wifi_net.cpp`, and `oracle/`.
- MIT = the rest of `runner/` — including the **2D compositor (`gpu2d.cpp`)**,
  the **SPU (`spu.cpp`)**, `bus/io/vram/scheduler/cp15`, `net/*` (host-side),
  `network_savestate_guard.*`, `title_patches`, the profiler/debug server.

Crucially, GPL is confined behind two clean C-style bridge headers
(`runner/src/gpu3d.h`, `runner/src/wifi.h`): `melonDS::NDS g_nds` exists **only**
in `gpu3d.cpp`, and the core (`bus/io/vram/gpu2d/spu/scheduler/state`) contains
**zero** `melonDS::` references. This isolation is why the GPL boundary stays
clean and auditable even though we now ship the melonDS device models.

## Decision (D7) — resolved

**Adopt `RetroPortingToolKit/ndsrecomp` pinned at tag `v0.6.10`
(`aa40b575b6f68a537f24ff3e4c94b567baa25587`) as the recompiler + runner
(its `recompiler` + `runner`) *as-is*, keeping its melonDS-derived 3D GE
(software + compute) and Wi-Fi, and license this project GPL-3.0-or-later.**

> This **supersedes** the earlier "delete the melonDS GPL subtrees and stay MIT"
> decision. The project deliberately migrated to GPL-3.0-or-later so the full,
> working 3D GE and Wi-Fi stack can be reused verbatim — deleting them would
> have removed *all* 3D rendering (soft and compute) and forced stubs.

Rationale:

- `v0.6.10` is the last tag whose **own** code is MIT; its melonDS trees are
  GPL-3.0-or-later. MIT is GPL-compatible, so the combined work is licensed
  GPL-3.0-or-later (a permitted one-way relicence). `main` is PolyForm
  Noncommercial and cannot be combined into anything.
- "Don't reinvent the wheel": the runtime/HAL is the expensive part of M2, and
  `ndsrecomp` already ships one (its recompiler handles autoload tables, static
  copies, ARM7 stack trampolines, relocated-code aliases, override tables, and a
  live-opcode fallback — exactly the architecture we would build).
- Its demonstrated titles do **not** include Gen 5 (firmware menu is the
  baseline), so expect real bring-up, not a turnkey boot.
- Prior in-house-recompiler plan (this file's old content) is **superseded**.

### Adoption architecture (option 1: faithful floor)

1. **Runner** = `ndsrecomp` runner: dual-CPU (ARM9+ARM7) scheduler, memory bus,
   MMIO dispatch, CP15/TCM, DMA, timers/IRQ, IPC/FIFO, cart/NitroFS, 2D/3D PPU,
   audio.
2. **Game code** = static banks generated from the user's ROM (`arm9.bin`,
   overlays at RAM addrs, ARM7 image) → C++ linked into the runner. Generated
   banks are immutable; we patch/extend via HLE wrappers + descriptors (never by
   editing generated code).
3. **Our integration** = feed NitroFS reads from the M1 `nds`/`narc` layer;
   keep the melonDS 3D GE (software floor by default, compute optional) and the
   ARM7 sound path enabled.
4. **Graphics + sound are part of the runner**, not stubbed by us: enable the
   2D compositor, the GE (3D added as the title scene needs it), and the ARM7
   sound path (recompiled driver + host audio device) from the start.

### BIOS (D8)

HLE the BIOS SWIs by default via `freebios` (BSD-2-Clause; no dump required). A
user-supplied BIOS stays an optional input, never shipped.

## References

- `RetroPortingToolKit/ndsrecomp` README / `PRINCIPLES.md` / `HLE_ARCHITECTURE.md`
  (architecture reference; licenses above). `v0.6.10` tree.
- Sonic Unleashed Recompiled / XenonRecomp (the direct analog of this approach).
- `N64Recomp` / Zelda64Recomp (the N64 analog).
- Ship of Harkinian / OoT decomp (the decomp-port family we cannot use).
- `mstan/freebios` (BSD-2-Clause); `AetiasHax/ds-decomp` (MIT).
- `overlays.md` (BLZ), `header.md` (load addresses), `arm9-init.md` (entry/init).

---

## Integration findings (2026-10-16) — v0.6.10, verified in-tree

Tool/ROM: `nds_recompile` + `nds_runner` @ `aa40b575` built with LLVM-MinGW
clang (Release, Ninja). Target ROM: Black USA/EU NDSi, 256 MiB.

### Corrections (2026-10-16, re-verified at HEAD)

- **The submodule hazard below is already resolved.** `git ls-tree HEAD
  third_party/ndsrecomp` returns `160000 commit
  aa40b575b6f68a537f24ff3e4c94b567baa25587` — the `v0.6.10` tag, not
  `origin/main` — and `HEAD:.gitmodules` carries both entries (the
  `third_party/ndsrecomp` one and the nested
  `third_party/ndsrecomp/external/arm-recomp-core` override). Only the working
  tree was ever wrong. The findings below are kept as the record of what was
  observed; treat their "must fix before the first push" as done.
- **The throughput figures in "First boot of Black" are void.** That run used
  `build/windows-debug` (`CMAKE_BUILD_TYPE=Debug`, 49 MB runner), so neither
  "~0.75 % of real-time" nor any timing derived from it describes an optimized
  build. Bring-up runs now use an optimized configuration; re-measure before
  drawing conclusions. (The 250 M-cycle run's own profile implies ~72 s wall,
  not the ~2491 s recorded here — an unexplained ~35× internal inconsistency in
  that log, another reason to discard the numbers rather than quote them.)

### Optimized build (2026-10-16) — the Debug caveat resolved

Same command, same ROM, `cmake --preset windows-relwithdebinfo
-DPKMNG5PC_BUILD_RUNNER=ON` (whole runner and every bank shard at `-O2 -g`):

| 30 M ARM9 cycles | Debug | RelWithDebInfo |
| --- | --- | --- |
| wall | 60.32 s | **4.98 s** |
| scheduler (emulation) | 16.358 ms/1000 rounds → 4.38 s | **6.332 ms/1000 rounds → 1.69 s** |
| fixed startup | ~55.9 s | **~3.3 s** |

- **Observation.** Marginal throughput on the optimized build is **~17.7 M ARM9
  cycles/s ≈ 26.4 % of real-time** (was ~6.3 % on Debug). Emulation is 2.6×
  faster and the fixed pre-scheduler cost is 17× smaller.
- **Observation.** Scheduler split at this budget is ARM9 2.515, **ARM7 2.387**,
  devices 0.946 ms/1000 rounds — the ARM7 is now ~38 % of scheduler time rather
  than the 71 % seen on Debug at 250 M, so coverage work still matters but is no
  longer the whole story.
- **Inference (resolves the startup-tax question).** The ~35 s ROM read and
  ~12 s SHA-1 both collapse into the ~3.3 s fixed cost once optimized. **No local
  patch series is warranted**; the vendored reader needs no change, and
  `third_party/ndsrecomp` stays pristine with no exceptions.
- **Observation.** `event_counts` exposes `insn9`/`insn7` and `cyc9`/`cyc7`
  alongside the vblank counters, which makes emulated-time progress measurable
  from the debug server alone (`tools/runwatch.py`).
- **Observation (smoke run, 60 M cycles, `--serve`).** At vblank9=60 the ARM9 is
  at `0x02076ED0` and at vblank9=120 at `0x020882D8` — different SDK addresses,
  not one spinning routine — while both engines report `bgA=0000 objA=0` and a
  uniform framebuffer (all 49152 pixels lit, identical hash on A and B, i.e. the
  backdrop with no layer enabled). Emulated `cyc9` at vblank9=120 is 156.5 M.
- **Unknown.** Whether a layer is ever enabled within tens of seconds of
  emulated time; the long `runwatch` run decides it.

### First visible frame (2026-10-16)

- **Observation.** `tools/runwatch.py` sampled every 60 vblanks and stopped on
  the first enabled layer (`banks/timeline-boot.jsonl`). At vblank9 = 60/120/180
  both engines report `bg=0000 obj=0` and the framebuffers are identical
  (`sha256 4ae2e6e6…`, all 49152 pixels lit — the backdrop, no layer). At
  **vblank9 = 240** (`cyc9 = 290,996,340` ≈ 4.34 s emulated) **BG1 is enabled on
  both engines** and the frames change: A `c63792f0001255…`, B `30343c48af6268aa…`.
- **Observation.** Frame A is the **Pokémon Company + Nintendo** boot splash,
  light-on-white as expected mid-fade; frame B is blank white. Written locally
  as PPM/PNG under git-ignored `banks/frames/` — ROM-derived output, never
  committed.
- **Inference (high).** The M2.3 gate is met: the game's own recompiled 2D path
  draws the boot splash, so BG setup, VRAM, palettes and the compositor are all
  live. The earlier "no BG/OBJ layer is ever enabled" finding was a **near
  miss** — that run reached ~223 vblanks; the layer turns on at 240.
- **Unknown.** Whether the sequence proceeds through the remaining logos to the
  title screen, and whether the ARM7 Tier-3 load (still ~38 % of scheduler time)
  starts to dominate as scenes get busier.

### The live window (2026-10-16)

Tool/ROM: `nds_runner` @ `aa40b575`, `windows-relwithdebinfo` with
`NDS_SDL_BACKEND=SDL3`, Black. Wrapper: `tools/play.ps1`.

- **Observation (source).** `--interactive` is the live path
  (`main.cpp:484`), and it is dispatched **before** `--serve`
  (`main.cpp:2261` vs `2287`), so passing both gives a window, not a headless
  server. The ARM9 cycle budget is **inert** under `--interactive`: it is
  consulted only in the default batch path (`main.cpp:2314`), and the frontend
  loop has no budget check.
- **Observation (source).** The interactive path **also** starts the debug
  server (`debug_pump_start(port)`, `main.cpp:2273`) but keeps execution
  frontend-owned: `run_to_*` is rejected, while queries, rings and key/touch
  injection stay live. So a running window is observable with
  `tools/ndsquery.py` — useful for the wedge investigation, since the window is
  the *only* driver that is not the debug protocol.
- **Observation (source).** Presentation is one window of 512×768,
  top-over-bottom by default (`NdsScreenLayout::Stacked`; `kWindowScale = 2`,
  `frontend.cpp:655`; window size at `frontend.cpp:1519-1533`).
  `--screen-layout separate` gives two windows. There is **no** window-scale,
  `--vsync` or `--fps` flag in the whole parser.
- **Observation (source).** Pacing is the **audio queue**, not a timer or a
  display sync: the throttle loop sleeps while the queue is above its floor
  (`frontend.cpp:1013`, "Audio is the host's real-time clock") and is only
  armed once `audio_started`. The headless `--serve` and default batch paths
  never call it and therefore have no pacing at all.
- **Observation.** The frontend honours `NDS_FRONTEND_MAX_FRAMES`
  (`frontend.cpp:2418`): it presents that many frames, then exits and prints a
  soak line carrying `fps`, `audio_started`, underruns and an FNV hash of each
  screen. That makes the window self-verifying without a screenshot.
- **Observation (measured, `banks/play-selftest.log`).** 600 frames in 12.166 s
  wall → **49.3 presented frames/s**, `audio_started=1`, 40 underruns;
  phase split `emu=5.217s present=0.816s drain=5.562s other=0.572s`.
- **Observation (measured, `banks/play-splash.log`).** 280 frames in 6.248 s →
  **44.8 frames/s**, `audio_started=1`, **zero** underruns, and
  `frame_fnv=(f462d3ff20fd83f4, 06be2b0d14dc4383)` — **different** top and
  bottom digests. At ≈4.7 s emulated that is the boot splash on the top screen
  with the bottom screen blank, i.e. the same state the debug-server capture
  recorded, now produced and shown by the live frontend.
- **Inference (high).** The M2.3 gate is confirmed through the live path, not
  only through `--serve`: the recompiled 2D path reaches a real SDL window and
  the presentation layer is faithful to the captured frame.
- **Correction.** The repo's "**~26 % of real-time**" is not a frame-rate
  ratio. It divided *ARM9 cycles* by wall time, and ARM9 cycles stall while the
  CPU is halted (`run_cycles` returning `reached=false` on a halted CPU is the
  same fact), so the ratio has no fixed relation to throughput. The honest
  measure is the presented-frame rate, and it is **~45–50 fps against the
  guest's 59.8261 fps** over these first ~5–10 s of emulated time.
- **Unknown.** The presented-frame rate through the intro itself, where the
  ARM7/overlay Tier-3 load is far heavier. The headless intro run took 392.1 s
  wall for 83.675 s emulated (~21 %), but that figure includes a debug round
  trip and a screenshot hash per sample, so it is a lower bound rather than a
  ratio. Whether the window **freezes** at the ≈83.7 s wedge (rather than
  exiting) is likewise unmeasured; the frontend has no budget check to exit on.

### `vblank9` is a guest-controlled counter, not a progress measure (2026-10-16)

- **Observation (source).** `io.cpp` increments `g_counts.vblank9` only inside
  `if (g_dispstat[0] & 0x0008u)` at `g_vcount == 192` — it counts *VBlank IRQs
  actually raised*, which requires the ARM9 to have DISPSTAT bit 3 (VBlank IRQ
  enable) set. `nds_event_value("vblank9")` and `io_state`'s `num_frames` both
  read that same counter. The raster itself is independent: VCOUNT starts
  sweeping at t≈0.03 s, and `vbIRQ` is still 0 until t≈0.37 s.
- **Consequence.** A harness that advances the guest with
  `run_to_event event=vblank9` stops making progress for as long as the guest
  has that IRQ disabled — normal across scene transitions and fades. The
  submodule's own probes are unaffected because the firmware paths they target
  leave VBlank IRQs enabled throughout. Treat `vblank9` as a *guest* signal.
- **Correction to the correction.** The first reading of the `vblank9=4980` stop
  — that it was only this counter standing still — is **itself wrong**. A run
  driven by `run_rounds` (immune to guest IRQ state) drives straight past
  `vblank9` 4980, reaching 4986 at t=83.67 s with `vbIRQ=1`, and *then* a single
  `run_rounds` of 37 000 rounds — 0.2 s of work earlier in the run — **did not
  return within an hour**. Two independent drivers, told apart only by this
  experiment, both stop at t≈83.7 s. **The machine really does stop
  progressing**; see the wedge section below. The counter semantics are a real
  trap, but they were not the cause here.
- **Method.** `tools/runwatch.py` advances on scheduler rounds (`run_rounds`),
  which no guest state can stall, and treats VCOUNT (`0x04000006`) and DISPSTAT
  (`0x04000004`) as ground truth, with a per-command timeout so a driver call
  that fails to return is *reported* rather than burning an hour. `run_cycles`
  is not a safe driver either: the ARM9 cycle counter stops while the CPU is
  halted, so it returns `reached=false` on any halted CPU — which is how a
  cycle-driven attempt ended at t≈4.15 s with the raster demonstrably alive
  (VCOUNT changing, `vblank9` 228→246 across the probe, `cyc9` +20 M in one
  call).

### The submodule pointer must record the tag, not `main`

- **Observation.** `git submodule status` prints `+aa40b575… (v0.6.10)`: the
  submodule's checked-out HEAD is tag `v0.6.10`, but the **staged gitlink**
  (`git ls-files -s third_party/ndsrecomp`) is
  `3a57236bb23d25dcb4caad7d58d733311062ff5e`, which is `origin/main` ("Merge pull
  request #26 … hle/mkds-vector-20261004"). The two commits are unrelated —
  neither is an ancestor of the other (`git merge-base --is-ancestor` exits 1) —
  and only `aa40b575` carries the `v0.6.10` tag.
- **Inference.** Committing the gitlink as staged would pin the project to
  rolling `main`, contradicting every place this repo records `v0.6.10` — and
  specifically the licence basis: `v0.6.10` is the last tag whose own code is MIT
  (see above), while `main` is PolyForm Noncommercial. The pointer must record
  `aa40b575b6f68a537f24ff3e4c94b567baa25587` before the first push.
- **Observation.** The same applies to `.gitmodules`: the staged copy has only
  the `third_party/ndsrecomp` entry, while the working tree also carries the
  `third_party/ndsrecomp/external/arm-recomp-core` entry that fresh clones need.

### The runner is LLE, not HLE

- **Observation.** Grepping the runner for `FS_OpenFile`/`FS_ReadFile`/`OS_*`
  finds nothing; `docs/dispatch_architecture.md:103-108` states "No load-bearing
  HLE and no interpreter on the Tier-1 hot path"; `ISSUES.md:569` calls the
  "LLE-faithful floor … untouchable. No stubbed SWIs". The only HLE is opt-in
  per-title CPU-math replacement (`HLE_ARCHITECTURE.md`), driven by a
  `--hle-manifest` whose schema is exactly `version` + `[program]` +
  `[[routine]]` (`recompiler/finder/config.cpp:433-457`).
- **Inference.** No NitroSDK/FS/OS HLE exists or is needed. The game's own
  NitroSDK code is recompiled, and the cart interface is emulated. Our M1
  `nds`/`narc` layer therefore has **no runtime role** — it is an offline
  library (bank generation now; save I/O and launcher detection later). Plan
  M2.2 was rewritten accordingly.

### There is no retail-title precedent

- **Observation.** `README.md:64-66`: "SM64DS, Mario Kart DS, and Prime Hunters
  are separate title repositories with their own exact-ROM gates, generated
  banks, and release status." No SM64DS banks/config/generator exist in-tree;
  `title_patches.cpp` is compiled C++ keyed on hardcoded addresses
  (`runner/src/title_patches.cpp:15`), not a per-title data directory.
  Glob `**/title_patches/**` and `**/registration-order*.txt`: no files.
- **Observation.** `recompiler/finder/config.cpp:2` and `config.h:4` reference
  `docs/TOML_SCHEMA.md`; the file does not exist. The authoritative schema is the
  parser: `[program]` requires `load_address`/`size`/`entry_pc`; `[identity]`
  requires `sha1` (of the exact `--bin` bytes, verified at
  `config.cpp:650-692`); `[[entry_point]]`, `[[data_range]]`, `[[code_copy]]`,
  `[[jump_table]]`, `[[exclude_func]]` are optional.
- **Inference.** We are the first to bank a Gen 5 title; expect bring-up, and
  expect to discover the working config shape ourselves.

### Banking mechanics that constrain the tool

- **Observation.** `recompiler/src/main.cpp:1621-1630`: a `_NN` shard suffix is
  only appended when `shards > 1`. The runner globs bodies as
  `runner/CMakeLists.txt:137-138` `*_[0-9][0-9].c`, so **`--shards 1` produces
  `<bank>.c` and is silently ignored** — minimum `--shards 2`. Every source name
  must also contain `_arm9_`/`_arm7_` (`:256-265`, CPU is inferred from the
  filename), and a group missing bodies *or* dispatch is fatal (`:175-179`).
- **Observation.** Banks are gated by exact ROM SHA-1
  (`NDS_TITLE_ROM_SHA1`, `runner/src/main.cpp:2130-2134`) and the bank directory
  is **compile-time only** — there is no CLI flag. The runner must therefore be
  configured *after* banks exist; bank generation is a pre-configure step.
- **Observation.** `FunctionFinder::run` has a default `max_functions = 4096`
  (`finder/function_finder.h:185`) and `recompiler/src/main.cpp:1431` calls it
  with no argument, but **Black's ARM9 reports 5265 discovered functions**, so
  the cap did not bind on this path. No fork is required (D-question resolved:
  accept). ARM7 reports only **49** functions — most ARM7 code will run through
  the live interpreter.
- **Observation.** Overlays have no static bank pipeline; the runner's design is
  runtime live-overlay compilation with content-validated dispatch rows
  (`docs/overlay-strategy.md:191-215`, `runner/src/live_overlay.cpp`). Static
  per-overlay banks are research-only. We bank ARM9 + ARM7 only.

### ARM7 discovery is thin because the image relocates into WRAM

- **Observation.** Recompiling the ARM7 image from `entry_pc = 0x02380000`
  discovers only **49 functions**; the finder reports 2 unresolvable indirect
  targets in `0x03800000..0x03810000`, and `stored_ptr_seeds=0`.
- **Inference.** `0x03800000` is ARM7 WRAM. The ARM7 image copies part of itself
  into WRAM and dispatches there — the same "ARM7 runtime-RAM bank" pattern the
  recompiler's own notes describe for SM64DS. Discovery cannot follow a code copy
  nobody declared, so most ARM7 code falls through to the live interpreter.
- **Unknown.** Whether interpreter speed is enough for the ARM9↔ARM7 FIFO
  handshake to complete in usable time. The config-level fix, if it is not, is to
  declare the copy (`[[code_copy]]`, or `[[data_range]]` plus `[[entry_point]]`
  seeds in a per-title hint file) and regenerate. Deliberately deferred until a
  first boot shows whether it matters.

### The 2D path is further along than its own issue tracker says

- **Observation.** `docs/accuracy_burndown.md:275-299` claims 2D is "pixel-exact
  and full speed on the exercised firmware paths" but lists windows as
  unchecked, and `ISSUES.md:553` says Win0/Win1/OBJ-window/WINOUT are
  "unimplemented". `runner/src/gpu2d_window.h:34-52` implements all of them and
  `gpu2d.cpp:1145-1172` calls into it.
- **Observation.** Main-memory display FIFO/capture is genuinely missing
  (`ISSUES.md:554`; `gpu2d.cpp:1201,1352,1979`).
- **Observation.** `gpu2d_window_test` — the suite for exactly this code — exits
  `0xC00000FD` (`STATUS_STACK_OVERFLOW`) when run directly in this Debug build.
  `runner/CMakeLists.txt:728-733` gives `nds_runner` a 64 MB stack
  (`-Wl,-stack_size,0x4000000`) *because* the dual-CPU boot nests deeply, but the
  test target is not given that flag. So the "all of them implemented" claim rests
  on the source, not on a passing on-Windows test.
- **Observation.** `live_index_lock_test` (Python-driven, driven from
  `runner/CMakeLists.txt:1084-1086`) fails in this environment with
  `index writer failed with 1`. Unrelated to the banks.
- **Inference.** A 2D boot logo is plausibly renderable; the docs' issue list is
  stale, so treat runtime behaviour as the authority.

### Build shape

- **Observation.** ndsrecomp has **no root `CMakeLists.txt`** — `runner/` and
  `recompiler/` are standalone projects. The runner resolves SDL3 with
  `find_package(SDL3 CONFIG REQUIRED)` (`runner/CMakeLists.txt:693`) with no
  FetchContent, so a parent configure must supply SDL3. Neither CMakeLists
  references `CMAKE_SOURCE_DIR`/`PROJECT_SOURCE_DIR`, so `add_subdirectory` is
  safe.
- **Observation.** `docs/BUILD_PERFORMANCE.md:38-39` records a reference corpus
  of 466 generated C files / ~1.32 GiB of source. Our Black ARM9 + ARM7 banks are
  ~33 MB across 16 body shards + 2 dispatch files — three orders of magnitude
  smaller, so the build is not the risk the docs imply.

### Generated artifacts (this session)

| Bank | Source | Load | Entry | Functions | Shards |
| --- | --- | --- | --- | --- | --- |
| `irbo_arm9_main` | `0x6F898` BLZ → `0xA67C0` | `0x02004000` | `0x02004800` | 5265 | 8 |
| `irbo_arm7_main` | `0x28F84` raw | `0x02380000` | `0x02380000` | 49 | 8 |

Hashes (SHA-1), independently re-verified with an external tool:

- ROM `26ad0b9967aa279c4a266ee69f52b9b2332399a5`
- decoded ARM9 `fd4edf3f83b602e79e41ccaaec2270364178a4dd`
- decoded ARM7 `364259352fffdcd15b75c2d9e997d2ed0d877190`

### First boot of Black (2026-10-16)

Verified command (repo root; the two positionals are the BIOS scratch directory
then the ARM9 cycle budget):

```
build/windows-debug/third_party/ndsrecomp/runner/nds_runner.exe \
    bios 250000000 --boot direct --generated-firmware --freebios --no-save \
    --rom "roms/<your-rom>.nds"
```

- **Observation.** Without `--freebios` the run aborts before executing anything:
  `[load] arm9 bios: missing/empty`, `[load] arm7 bios: missing/empty`,
  `refusing to start: dump verification failed`. With it, the run proceeds.
  `main.cpp:1565-1572` and `1573-1581` show `--freebios` and
  `--generated-firmware` each require `--boot direct`; only retail BIOS +
  firmware dumps unlock `--boot lle`.
- **Observation.** Bank registration works at runtime:
  `[dispatch] registered configured title banks for
  26ad0b9967aa279c4a266ee69f52b9b2332399a5`, then
  `Dispatch arm9: total=2615052` / `arm7: total=1545427`. The generated banks
  are live, not dead code.
- **Observation.** At 250 000 000 ARM9 cycles (~7.5 s of emulated time,
  2.49 M scheduler rounds, ~2491 s wall) the run is at **~0.75 % of real-time**:
  - ARM9 `PC=0x02004E3C`, `LR=0x02079114`, ARM mode, inside an SDK copy/expand
    routine spanning `0x02004E28-0x02004E74` (disassembled from
    `banks/black/bin/irbo_arm9_main.bin` with `llvm-objcopy -I binary` +
    `llvm-objdump`). It is doing real work, not a hard hang: the destination
    pointer advanced from `0x02159F20` (at 30 M cycles) to `0x021603E0`.
  - ARM7 `PC=0x00001158` (ARM7 BIOS), `IF=0x00000000`, `IE=0x01040099`: idling in
    BIOS on an interrupt, stack already set up at `0x0380FB68` (ARM7 WRAM),
    `LR=0x037F80E8`.
  - GPU2D composited 42816 scanlines but reported `A text[0..4]=0/0/0/0/0`,
    `B=0/0/0/0/0`, `direct 0 frames`: **no BG or OBJ layer was ever enabled**, so
    nothing is displayed. M2.3's logo gate is **not** met.
- **Observation.** The cost is ARM7-dominated: `Scheduler profile: 28.794 ms/1000
  rounds (ARM9 4.789, ARM7 21.065, devices 1.599)`, with the ARM7 dispatcher at
  `cache_hit/absent/slow=186482/37107038/7598` (37.1 M absent lookups).
- **Observation.** The coverage manifest
  (`roms/...-coverage-20261016-015352-part00.json`) reports `tier3_entries9=80329`,
  `tier3_entries7=1374176`, `tier3_insns9=3173756`, `tier3_insns7=34366147`.
  A large share of executed code falls through to the tier-3 interpreter - some
  of it at ARM9 addresses *inside* the bank's own `0x02004000-0x020AA7C0` range
  (e.g. `0x02005640`, thumb, 201 hits).
- **Inference (medium).** The ARM7 stays in BIOS because `--boot direct` does not
  execute the firmware (`[boot] direct boot (opt-in): cartridge entry state
  established, firmware not executed`), and its wait loop needs a VBlank IRQ
  that only appears once the ARM9 enables the display - which it has not done
  yet. The two observations are therefore consistent with ordinary early boot,
  not a proven deadlock.
- **Inference (medium).** Reaching game-drawn logos needs the ARM9 to finish
  megabytes of SDK/file-system init *and* to run overlay code (Gen 5 game logic
  lives in `ovNNN`). With most of that interpreted, the current speed puts the
  logos well outside this session.
- **Unknown.** Why so much in-range ARM9 code falls to tier-3; whether the
  live-overlay JIT (`--live-overlay-enable --live-overlay-command gcc`; the
  LLVM-MinGW `gcc` wrapper exists on this machine) changes the picture; whether
  an ARM7 `[[code_copy]]`/WRAM declaration materially cuts the interpreter load.

Confidence: high on all observations (read from source or produced by the tool
this session). The boot above resolves "the correct boot flags for this title"
(`--boot direct --generated-firmware --freebios`); the M2.3 expectation is now a
measured negative. Unknowns: whether the 49-function ARM7 bank slows the FIFO
sync below usable speed; whether the emulated cart path suffices for the game's
own file reads; why in-range ARM9 code falls to tier-3.

### Re-measurement (2026-10-16, same day) — supersedes the numbers above

Same flags, same (Debug) configuration; logs in `banks/bench-*.log`.

| ARM9 budget | wall | scheduler emulation | marginal cyc/s | % of 67.03 MHz |
| --- | --- | --- | --- | --- |
| 10,000,000 | 53.2 s | 1.5 s | — | 0.28 % |
| 30,000,000 | 60.3 s | 4.4 s | — | 0.74 % |
| 250,000,000 | 112.8 s | 59.4 s | **4.19 M** | **6.26 %** |

- **Observation.** "~2491 s wall" above is a ~22× error: the same scenario takes
  **112.8 s**, and the log's own profile figure was the closer one. "~0.75 % of
  real-time" describes a ~30 M run, not a 250 M one. Marginal (steady-state)
  throughput is ~6.3 % of real-time.
- **Observation.** ~52 s of *every* run is a fixed cost incurred before the
  scheduler starts: ~35 s reading the 256 MiB ROM through
  `std::istreambuf_iterator` (`runner/src/main.cpp:141-146`; ~7.3 MiB/s against
  803 MiB/s for the same read via the OS) and ~12 s in the runner's hand-rolled
  SHA-1 over it (`main.cpp:1695`; ~21 MiB/s). Two host-side operations that
  should together be well under 2 s.
- **Observation.** Every runner and bank object in this build was compiled
  **`-O0`** (`build/.../nds_runner.dir/src/main.cpp.obj: ...
  CXX_COMPILER__nds_runner_unscanned_Debug`, `FLAGS = -g ...`, no `-O`), so the
  absolute magnitude of every figure here is inflated. Nothing in these numbers
  separates "Tier 3 is inherently slow" from "this build is unoptimized".
- **Observation (corrects the entry above).** The ARM7 is **not** idling in BIOS
  as an interpreted loop. `freebios_arm7` *is* registered under `--freebios` and
  carries an exact dispatch row for `0x00001158`
  (`generated/freebios_arm7_dispatch.c`), and the ARM7's Tier-3 histogram holds
  **zero** BIOS entries. Its cost is **unbanked WRAM code**: 1.27 M of its
  1.374 M Tier-3 entries are ARM7 WRAM (`0x0380xxxx`, hottest `0x03806BA4`) and
  the shared-WRAM view (`0x037Fxxxx`, hottest `0x037FE558`), which is 71 % of the
  250 M run's scheduler time.
- **Observation.** The hottest in-range ARM9 Tier-3 PC is `0x02019A7C` (thumb,
  `call`, 91,917 hits), which appears **nowhere** in
  `banks/black/irbo_arm9_main_dispatch.c` — a **discovery** gap, not a
  relocation one. 192 of 210 ARM9 Tier-3 entry points lie inside the banked
  range (`0x02004000-0x020AA7C0`).
- **Inference.** The two coverage gaps are distinct and both config-level:
  relocated ARM7 code needs `[[code_copy]]`/seeds; undiscovered in-range ARM9
  entry points need seeded `[[entry_point]]`s. Feeding the coverage manifest
  back into `pkmn_bankgen` closes the second gap with no recompiler fork.
- **Unknown.** How much emulated time the ARM9 needs to reach the first enabled
  layer. At 250 M cycles (3.7 s of DS time, not the 7.5 s claimed above) it is
  still inside one banked SDK expand routine (`0x02004E28`, caller
  `LR=0x02079114`) having emitted ~25.8 KiB, and no BG/OBJ layer has been
  enabled at any point. Whether that routine is legitimately that expensive or
  pathologically slow in emulated time is unresolved.

### M2.3 met on the optimized build: the whole intro renders (2026-10-16)

- **Observation.** Driven on scheduler rounds (`tools/runwatch.py`, optimized
  `windows-relwithdebinfo` runner), the ARM9 enables BG1 at `vblank9=240`, the
  splash appears, and the intro continues through the Game Freak logo, the
  castle/throne scene, Zekrom, the cast and the "You and Your Pokémon!" card:
  **146 distinct presented frames**, hashed and written as PNG under the
  git-ignored `banks/` (`banks/screenshots/`, `banks/contact-A.png`, a
  top-over-bottom contact sheet). No frame is committed.
- **Observation.** The same stretch is produced and shown through the live SDL3
  path (`tools/play.ps1` + `NDS_FRONTEND_MAX_FRAMES`), so the frames are
  presented, not merely captured.
- **Inference (high).** M2.3's gate (pixels, not fps) is met well beyond the boot
  splash: BG/OBJ setup, VRAM banking, palettes, the compositor and the scene
  scripts all run from recompiled code. The remaining distance to M2.5 is the
  scene load that starts around t≈82.8 s, which is where the run currently stops.

### The boot wedges at t≈83.69 s: a host-side spin inside one scheduler round (2026-10-16)

- **Observation (reproducible).** A run driven by `run_rounds` stops making
  progress at **t≈83.69 s emulated** (ARM9 `cyc9` ≈ 5.609e9). It reproduces at
  the same emulated instant with 400-, 2000-, 3000- and 37 000-round call sizes.
- **Observation (localized to one round).** `banks/probe_step.py` advances one
  round per call and records both PCs. The last round that returns is **#42793,
  t=83.692661 s** — ARM9 `pc9=0x1FF8098` (ITCM), `lr9=0x1FF8044`, `r0=0x80`,
  `romctrl=0`; ARM7 `pc7=0x001158` (freebios, asleep). The *next* `run_rounds`
  call never returns, and a call is only a loop of bounded rounds, so **the wedge
  is inside a single round**.
- **Observation (a host spin, not a block).** With the call in flight, Win32
  `GetProcessTimes` sampled from outside the process gives **57.1 CPU-seconds over
  29.5 wall-seconds = 193 % of one core** (`banks/wall-calls.jsonl`). The process
  burns CPU; it is not waiting on I/O.
- **Observation.** Nothing is printed at the wedge: no `[dispatch-miss]` (so it is
  not an unbanked jump), no stub flood (the log holds 64 `0x04004010` stub reads
  and nothing else), no unimplemented-op abort.
- **Observation.** The 64 rounds before the wedge are ordinary execution — ARM9
  walking banked `0x0206xxxx`/`0x0208xxxx`, the ITCM span and the ARM9 high
  exception vectors (`0xFFFF0018`, `0xFFFF06F4`) — with emulated time advancing
  ~1.7 µs per round, and the ARM7 asleep in freebios throughout. There is no
  preceding guest spin to blame.
- **Observation (the ITCM span has three versions).** The runner's own manifests
  carry **three** versions of page `0x01FF8000`: two essentially empty (39
  non-zero bytes) and one loaded (1761 non-zero bytes — a real NitroSDK routine,
  `bl` at `+0x40`, `add r1,r1,#0x04000000` IO stores). The loaded version is the
  code executing at the wedge.
- **Inference (medium).** The wedge is a defect in the vendored runner's round
  path, not a guest deadlock. The slice backstops are *not* the culprit: the
  dispatch guard (`scheduler.cpp:273`, 20 M), the Tier-3 guard (`tier3.cpp:627`,
  50 M) and the ARM7 catch-up bail (`scheduler.cpp:486`) all bound their loops, so
  whoever this is, it never reaches one of them.
- **Observation (what it is not).** Not the ARM9 cycle budget (inert under
  `--serve`, `main.cpp:2287`), not `vblank9` (guest-controlled), not stub logging.
- **Observation (resolved — it is the gpu2d worker pool).** A/B with the 2D raster
  put back on the emu thread — `NDS_GPU2D_THREADED=0` (`main.cpp:1505-1520`, "0 is
  not a separate code path") — drives **clean through the wedge to t=95.03 s** in
  130.8 s wall (`banks/probe-wedge-threadedoff.log`, verdict `clean`), while the
  default (`=1`, 2 workers) wedges exactly as above. `banks/probe_wedge.py` does
  the same drive and samples CPU **per thread** during the stall, so the emulation
  thread can be told apart from the workers that spin while idle.
- **Inference (high).** The wedge is a defect in the vendored `gpu2d` threaded
  scanline path — the emulation thread is the *producer* of the per-line latches
  and its round does not return while a worker is involved. CPU attribution cannot
  distinguish it from idle worker spin, which is why the A/B, not the 193 % figure,
  settles it.
- **Workaround (in force; no fork).** All long runs set `NDS_GPU2D_THREADED=0`.
  The runner documents the inline path as the identical latch/execute sequence and
  the threaded path as byte-locked to it by the presented-frame digest ring
  (`main.cpp:1497-1504`), so this trades wall time for correctness, not pixels.
- **Action.** Report upstream carrying the round-level reproduction (round #42793,
  t=83.692661 s, `NDS_GPU2D_THREADED` A/B, per-thread samples); the vendored tree
  stays pristine meanwhile.

### Coverage: the submodule's own ingest names the missing bases (2026-10-16)

- **Observation.** `third_party/ndsrecomp/tools/ingest_coverage_manifests.py`
  consumes the runner-written manifest directly (schema 4: `root_map` + a `pages`
  section). Run over our seven manifests, with a prepared images directory
  (`banks/coverage-ingest/images/` — decoded `arm9.bin`/`arm7.bin` plus the two
  declared-base TOMLs, git-ignored) and both generated dispatch files as
  `--owned-rows`, it reported:

| item | value |
| --- | --- |
| manifests accepted / rejected | 7 / 0 |
| page versions resolved to a module | 70 of 87 |
| page versions **not** in the ROM | 17 (runtime-materialized) |
| merged entry points | 2427 |
| span seeds written | 1910 (arm9 874, arm7 1036) |

- **Observation (the ARM7 alias, exactly).** Two ROM-alias findings, both for
  `arm7.bin`, whose only bank is compiled at the header address `0x02380000`:

| module | resident base | declared base | pages | guest span | module span |
| --- | --- | --- | --- | --- | --- |
| `arm7.bin` | `0x02FCE5A8` | `0x02380000` | 11 | `0x02FE0000..0x02FF7000` | `0x11A58..0x28A58` |
| `arm7.bin` | `0x037F79BC` | `0x02380000` | 14 | `0x037F8000..0x03809000` | `0x644..0x11644` |

  Each base is exactly `guest_span_start - module_span_start`, so the fix needs no
  dumped bytes at all: recompile the same `arm7.bin` again with `load_address` set
  to each resident base. The submodule reports the same shape as 93.5 % of the
  first real player submission — the whole ARM7 module running from aliases no
  bank declares, hence 100 % of ARM7 execution interpreted.
- **Observation (entry points by target).** `arm7@0x037F79BC` 840,
  `arm7@0x02FCE5A8` 547, `arm9@0x02004000` 937, unresolved 98 (arm9) + 5 (arm7).
  The `arm9@0x02004000` seeds are discovery holes *inside* the declared range —
  the same class as the `0x02019A7C` finding above, now enumerated — and the
  hottest cluster is the interpreted block at `0x020834E8..0x02083590`, which a
  grep over `banks/black/*.c` confirms no generated function covers (every
  instruction address inside a bank function appears as a `case` label, so the
  absence is conclusive rather than incidental).
- **Inference (high).** Both long-open coverage gaps are now *specified* rather
  than guessed: the ARM7 alias needs two alias banks, the ARM9 holes need seeds.
  Neither needs a recompiler fork and neither needs anyone's dumped bytes.
  **Update (2026-10-16):** the ARM9 half is implemented and boots (5265 → 7175
  functions); the ARM7 aliases are **not** usable as-is — declaring them hangs the
  boot, so they are staged in `re/config/black.toml` but commented out. See
  §"The ARM7 alias banks are what stop the boot".
- **Unknown.** How much Tier-3 load either change removes, and whether the
  `0x01FF8000` ITCM span (runtime-materialized, so not a ROM alias) needs its
  captured-bytes bank — which is ROM-derived output and therefore stays in the
  git-ignored area, regenerated locally from the user's own runs.

### M2.5 gate: the title's A press opens the main menu (2026-10-16)

- **Observation (the title).** Engine A carries the title screen - the
  "Pokemon BLACK VERSION" logo, a `PRESS START` prompt, the Zekrom model and
  "Developed by GAME FREAK inc." - with engine B blank, from about t=104 s.
  `banks/shots-title/` holds 1647 montages out to t=169.87 s, but that capture
  comes from a keyless `tools/runwatch.py` run, which is why it never leaves the
  title: nothing was injected. Its log ends on the runwatch timeline line, and
  the title runs on into the attract sequence - not a menu.
- **Observation (the menu).** In the `banks/probe_start.py` run captured as
  `banks/shots-start/`, a fade begins at 120.44 s, one frame is blank at
  122.36 s, and from **t=122.70 s to t=136.48 s** the main menu is on engine A:

  | entry | state |
  | --- | --- |
  | NEW GAME | highlighted (selection bar) |
  | MYSTERY GIFT | |
  | NINTENDO WFC SETTINGS | wireless icon |
  | MIC TEST | |

  Engine B stays blank, the trainer art is composited behind the list, and the
  text is legible, so the font/decompression path renders
  (`banks/shots-start/t00131.71s-ds.png`; engine A alone in
  `t00131.71s-A.png`). There is no CONTINUE entry, which is correct for a run
  started with `--no-save`.
- **Observation (reproduced; A opens it, Start does not).** Re-driven on the
  optimized runner (`banks/probe_menu.py` -> `banks/probe-menu.json`,
  `banks/shots-menu2/`): **Start** (mask `0x3F7`) held 105.04-107.04 s changes
  nothing, while **A** (mask `0x3FE`) held 109.046-111.046 s is followed by a
  white flash at 109.12 s (all 49152 sampled engine-A pixels lit), a fully black
  frame at 111.35 s, and the same four-entry menu from **t=111.69 s** to the end
  of the run at 126.01 s. So the title -> menu delay is about 2.4 s: a "press
  start" beat, then a fade. The `keys` mask is the active-low KEYINPUT layout
  (`runner/src/io.cpp:2063` and `frontend.cpp:856`), so A is bit 0 and Start
  bit 3.
- **Observation (navigable, on the live window).** `banks/probe_live.py` ->
  `banks/probe-live.json`, montages in `banks/shots-live/`. That run is decisive
  for two reasons: it drives a **live** `--interactive` window (the M2.3
  precedent) with no driver thread of its own - under `--interactive` the runner
  *pumps* the debug server (`main.cpp:2273`) - and it injects keys through the
  **frontend's own** handler (`frontend_input` `action=key`, `frontend.cpp:3839`,
  i.e. real SDL `KEYDOWN`/`KEYUP` into the same path the keyboard uses, not a
  `keys`-mask shortcut). It is closed-loop: it polls engine A until the title
  profile appears (`lit_px` 25207, 164 colours at wall 100.6 s), presses `z` (A),
  polls for the menu profile, and only then walks the cursor.

  | montage | press before it | engine A | engine-A sha256 |
  | --- | --- | --- | --- |
  | `01-menu-01` | the press that opens it | NEW GAME selected | `6a2913c0ff842d29...` |
  | `02-down1-02` | Down | MYSTERY GIFT selected | `9ed9d60c6c63d3bb...` |
  | `03-down2-03` | Down | NINTENDO WFC SETTINGS selected | `d17237f30344d7af...` |
  | `04-up1-04` | Up | MYSTERY GIFT selected again | `e89784ac1d4be49d...` |
  | `05-activate-05` | A | MYSTERY GIFT's own menu - RECEIVE GIFT / CHECK THE CARD ALBUM / INFO / QUIT, RECEIVE GIFT selected - with "Welcome to Mystery Gift!" over a cyan gradient on engine B | `9079d293b84fef30...` |
  | `06-down1-06` | Down | INFO selected | `db4e3026d525a905...` |
  | `07-down2-07` | Down | RECEIVE GIFT again (the list wraps) | `bf862390268712cf...` |

  The menu profile (`lit_px` 49039, 82 colours) arrives 2.9 s after the press
  (wall 101.0 s -> 103.9 s), matching the ~2.4 s beat + fade above. Engine A
  carries the trainer art behind the list and engine B is blank during the title
  and the main menu; opening the submenu is the first time in these captures that
  engine B carries game text.
  Re-run on the **rebuilt final banks** - arm9 7175 functions from the 937 ARM9
  seeds, arm7 49, the build left in place after the bisect below
  (`banks/probe-live2.json`, `banks/shots-live2/`) - it reproduces exactly: the
  menu, NINTENDO WFC SETTINGS (`d17237f30344d7af...`), the Mystery Gift submenu
  (`9079d293b84fef30...`) and the wrap back to RECEIVE GIFT
  (`bf862390268712cf...`) are bit-identical to the first run, which used the
  transplanted pre-regeneration archive. The four frames that differ are the ones
  carrying the animated cursor and the trainer art.
- **Observation (the menu acts even when a press is lost).** Both runs whose A
  press was ignored reach the **Mic Test screen** anyway, at the *next* press (a
  Down): title "Mic Test", a Jigglypuff, "Say something into the microphone. The
  Pokemon will start singing if it's working." and a BACK button, at t=124.25 s
  (`banks/shots-nav/`) and t=113.72 s (`banks/probe-nav2.json`). That screen is
  reachable only through the startup menu.
- **Inference (high).** **M2.5's gate is met.** Black's main menu *displays* and
  *is navigable*, from recompiled guest code, and the navigability was captured on
  a live window through the frontend's real input path.
- **Unknown.** Why a press is accepted sometimes and ignored others: two title
  presses were ignored (120.08 s in `banks/probe-nav.json`, 108.562 s in
  `banks/probe-nav2.json`) and two were accepted (109.046 s in
  `banks/probe-menu.json`, and 120.33 s in the `probe_start.py` run behind
  `banks/shots-start/`, whose press log was later overwritten). One accepted and
  one ignored press are 0.25 s apart in emulated time, across runs, with no
  identified difference in the driving method. A press therefore has to be
  confirmed from the pixels rather than assumed - `banks/probe_live.py` polls the
  frame profile before and after every press, and its retry loop was not needed
  (the menu was detected on attempt 1 of 4). Untested candidates for the cause:
  the guest latching presses through the key IRQ (`io.cpp:2069`) rather than
  polling KEYINPUT, and the title state machine sampling input only at one point
  in its own animation beat.
- **Caveat (measurement).** `runwatch.frame_stats` counts pixels that are not
  exactly `(0,0,0)`, and a blank engine B reads as a `(0,0,0)`/`(40,40,40)`
  checkerboard, so `lit_px` is not a proxy for visible content: the mostly black
  menu frame reports 49039 of 49152 "lit". Use the engine-A sha256 and the
  montages, not `lit_px`.

### Regenerating the banks with the coverage fixes as they stand stops the boot (2026-10-16)

- **Observation (what changed).** `banks/black/` was regenerated at 15:30
  (`banks/bankgen-black.log`) from the uncommitted working tree
  (`re/config/black.toml`, `tools/bankgen/main.cpp` +250 lines,
  `tools/stage_title_config.py`; the two `re/config/black/seeds/*_unresolved.toml`
  files were folded in and deleted), which implements the previous section's
  recommendation. The generated `banks/black/config/irbo_arm7_main.toml` now
  declares `[[code_copy]] runtime_start=0x02FE0000 source_start=0x02391A58
  size=0x00017000 name="arm7_alias_02fe0000"` and
  `runtime_start=0x037F8000 source_start=0x02380644 size=0x00011000
  name="arm7_alias_037f8000"`, plus 1387 ARM7 seed entry points. Those sources
  were compiled into `nds_runner.exe` at 15:34-15:39.
- **Observation (the A/B).** With them the ARM9 never leaves the crt0 relocated
  wait-stub (`re/notes/arm9-init.md` A9-09): `pc9` sits at `0x01FF8010` /
  `0x01FF8014` with `irq9=0 irq7=0 dma=0` and `vblank9=0` at t=40 s, and both
  engines stay on one frame hash `4ae2e6e6e32d...`. That hash is not unique to the
  failure - it is simply the nothing-drawn-yet uniform frame, which a healthy boot
  shows for its first seconds too (`banks/probe-live.json` read 1) - so the
  *persistence* is the diagnostic, not the value. `tools/play.ps1` reproduces it:
  `frame_fnv=(f5c5169d40c04383,f5c5169d40c04383)` against the known-good
  `(f462d3ff20fd83f4,06be2b0d14dc4383)`, ARM9 dispatch `total=5493067` against
  ARM7 `168056747`.
- **Observation (the runner is not implicated).** The same binary boots under
  `--force-tier3` - `vblank9` 41/101/223/355 at t~1/2/4.07/6.27 s, splash hashes
  `b45f04f74705` / `1ac6f38899e7` - and so does the pre-regeneration archive:
  linking the 11:38 `build/windows-debug/.../libnds_banks.a` into
  `build/windows-relwithdebinfo` yields a runner that boots and runs at about
  0.78x realtime. That is the build behind `banks/shots-menu2/`, above.
- **Inference (high).** The regression lives in the regenerated banks, i.e. in
  the uncommitted bankgen/config change - not in the runner, and not in the
  `NDS_GPU2D_THREADED=0` workaround.
- **Bisected 2026-10-16 (see the next section).** The two ARM7 `[[code_copy]]`
  aliases alone reproduce the hang, with the ARM7 seed files disabled; the seed
  entry points are not implicated. The change was left uncommitted in the working
  tree.
- **Method note.** The banks are compiled *into* `nds_runner.exe`
  (`Runner.cmake`, `PKMNG5PC_BANKS_DIR`, default `banks/black`), so a bank-source
  change needs a runner rebuild, not just a regeneration. The archive swap above
  is a build-directory state only; `cmake --build build/windows-relwithdebinfo
  --target nds_banks` restores the current sources. `tools/play.ps1` is safe to
  run against that state: it never regenerates banks, it only fails when
  `banks/black` holds no `*_NN.c` bodies and prints the `pkmn_bankgen` command to
  run (`tools/play.ps1:69`); the message text at lines 72-73 is that instruction,
  not an invocation.

### The ARM7 alias banks are what stop the boot (2026-10-16)

- **Observation (three-way bisect, one runner binary).** Turning off only the two
  `[[alias]]` blocks in `re/config/black.toml` and regenerating each time
  (`banks/bankgen-bisect.log`, `banks/bankgen-bisect2.log`,
  `banks/bankgen-final.log`):

| config | arm7 bank | arm9 bank | boot |
| --- | --- | --- | --- |
| pre-regeneration, 01:58 (`banks/bankgen-verify.log`) | 49 fns | 5265 fns | healthy |
| aliases on + ARM7 seeds on (15:30) | 3567 fns | 7175 fns | **hang** |
| aliases on, ARM7 seed files off | 236 fns | 7175 fns | **hang** |
| aliases off (all seed files on) | 49 fns | 7175 fns | healthy |

  "Hang" is the state in the previous section (`pc9=0x01FF8010`, `vblank9=0`,
  `pc7=0x00001078`, `irq9=0 irq7=0` at t=40 s). "Healthy" is `vblank9` 41 -> 2376,
  the splash hash `b45f04f74705` at t=6.27 s and the intro frame at t=40 s
  (`lit=43520`), read with `banks/fbprobe.py`.
- **Observation (the seeds cannot be isolated from the aliases).** `bankgen` keeps
  a seed only when it lies inside the image or inside a declared copy
  (`tools/bankgen/main.cpp:486-501`), and all 1387 ARM7 seeds lie in the two alias
  ranges. With the aliases off they are all dropped - "0 seed(s), 1387 dropped
  outside image" - and the ARM7 bank returns to exactly the original 49 functions.
  So "aliases off" and "ARM7 seeds off" give the same bank, and the discriminating
  run is "aliases on, seed files off": it still hangs, from a 236-function bank.
- **Inference (high).** The alias bank's own code is what hangs the boot. The 1387
  ARM7 seeds are not implicated: they only ride along inside the alias ranges, and
  the hang reproduces without them.
- **Inference (high).** The ARM9 half of the same change is sound: 5265 -> 7175
  functions from the 937 ARM9 seeds, and the boot is healthy with it in place.
- **Unknown (the mechanism).** Where the alias bank goes wrong. One concrete lead:
  in the healthy boot the ARM7 is *already* executing inside an alias range -
  `a7=0x037FBAF8` at t=1.02 s, inside `0x037F8000` + `0x11000` - so the range is
  where the ARM7 really runs, and there it runs correctly through the Tier-3
  interpreter. With the alias declared, the ARM7 never reaches that address
  (`a7=0x00001078` instead). Untested candidates: the CFG walk for a
  `[[code_copy]]` range emitting mis-keyed or wrong functions; the runner's
  bank/registration handling of a bank that also covers a copy range; and the copy
  declaration perturbing the *main* bank's dispatch for neighbouring addresses.
  This needs the alias path of the recompiler/bankgen looked at.
- **Tree state (2026-10-16).** `re/config/black.toml` keeps the whole in-flight
  change except the two `[[alias]]` blocks, which are commented out with the
  reasoning at the site; `banks/black` was regenerated from that config and
  `nds_runner` was rebuilt, so the binary on disk boots. The pre-bisect file is at
  `banks/backup/black.toml.working` (with a `.patch` of the same), and the seed
  files are untouched.

### All four titles boot to the intro and the 3D title screen (2026-10-16)

- **Observation (a bank per title).** `pkmn_bankgen` was run on the other three
  ROMs, each with **no per-title config** - there is no `re/config/<title>.toml`
  for them yet, so no `[[alias]]`/seed work - so each got the plain static bank.

  | title | game code | bank dir | ARM9 fns | ARM7 fns | bank ROM SHA-1 |
  | --- | --- | --- | --- | --- | --- |
  | Black | `IRBO` | `banks/black` | 7175 (5265 + 937 seeds) | 49 | `26ad0b99…` |
  | White | `IRAO` | `banks/white` | 5265 | 49 | `bc696a0d…` |
  | Black 2 | `IREO` | `banks/black2` | 5865 | 49 | `e51e6dfb…` |
  | White 2 | `IRDO` | `banks/white2` | 5865 | 49 | `b5d7490b…` |

  Evidence: `banks/bankgen-{white,black2,white2}.log`. The ROM SHA-1 each pair
  records as its `identity sha1` matches the ROM (`bankgen` prints it against
  the actual file), and the runner registers a group only when the booted ROM's
  SHA-1 matches, so a mismatched pair fails closed rather than half-running.
  White shares Black 2's ARM7 image (`36425935…` in both) and White 2 shares it
  too, the same sharing the static notes record for the pairs. Black's bank is
  the only one carrying per-title seeds; its numbers are the post-bisect ones
  above.

- **Observation (one runner, four groups).** `nds_runner` was then reconfigured
  with the runner's multi-title form - `NDS_TITLE_BANK_DIRS` /
  `NDS_TITLE_ROM_SHA1S`, the parallel lists with the three extra dirs next to
  Black's, matching its own `NDS_TITLE_BANK_DIR`/`NDS_TITLE_ROM_SHA1` - and
  rebuilt (`banks/build-4title.log`), so one binary carries all four bank groups
  and selects by the booted ROM's SHA-1.

- **Observation (ran fine, by hand, live).** The three extra titles were played
  on the live window (`--interactive` via `tools/play.ps1 -Rom <path>`): the
  intro cinematic runs through, and the **3D title screen** renders (the title's
  logo/legendary scene is drawn by the GE path), the same visible sequence Black
  shows. A headless `tools/runwatch.py` pass on White also runs past the intro
  (400 sampled states to t≈94.8 s, `banks/runwatch-white.out`,
  `banks/timeline-white.jsonl`).

- **Observation (B2W2's ARM9 bank shape).** The B2W2 ARM9 bank is 5865
  functions and Thumb-dominated (326 ARM / 5539 Thumb), against BW's 5265
  (4750 / 515) - an engine delta, not a coverage regression. The pair shares an
  ARM9 *size* band but not an image (Black 2's identity is `d01b69ba…`, White
  2's `19cb08a6…`), so they are banked separately.

- **Inference (high).** Banked boot needs no per-title work beyond running
  `bankgen` on the ROM; the boot/intro/title path is title-independent. The
  per-title config machinery (aliases, seeds) is a coverage refinement, not a
  precondition for the title to render.

- **Unknown / scope.** White, Black 2 and White 2 were **not** driven past the
  title screen - the A-press menu was validated on Black only. Whether the ARM7
  alias problem (above) and the `gpu2d` wedge behave the same on the other three
  was not measured, and no per-title coverage manifest was read. Linux remains
  unverified for all four.

### Bank patch mechanism (2026-10-18)

- **Observation (new directive).** `pkmn_bankgen --title-config` consumes `[[patch]]` blocks (address, kind, reason) and rewrites the emitted shard C after the recompiler emits it: `skip_call` replaces a BL/BLX block with a PC-advance fall-through, `stub_return` empties a function to a `bx lr`-equivalent.
- **Observation (declared).** `re/config/black.toml` declares one patch: `0x02075F7C skip_call`, the `lock_leave_raw` id-mismatch assert (`banks/HANDOFF.md` §7).
- **Observation (deterministic).** Regenerating Black with the config changed `irbo_arm9_main_04.c` only - every other generated file was byte-identical - and printed `[patch] 0x02075F7C skip_call applied (shard irbo_arm9_main_04.c)`.
- **Observation (fail-loud).** An address absent from every shard and a non-BL/BLX site both exit nonzero naming the patch and shard; an unknown kind exits 2 before any recompilation.
- **Observation (boot unaffected).** The rebuilt runner still renders the splash (`b45f04f74705` / `1ac6f38899e7`, t≈6.24-6.27 s). Whether NEW GAME now proceeds past the assert is not yet re-driven end-to-end. (The save-capable configuration is answered in *Save-loaded runs are playable; NEW GAME needs the save-capable build* below; the runner's EEPROM default is still open.)

### Save-loaded runs are playable; NEW GAME needs the save-capable build (2026-10-18)

Answers the open question at the end of *Bank patch mechanism* for the
save-capable configuration, and leaves the historical default open.

- **Observation (playable, save loaded).** With a battery save loaded, Black
  boots from the title into the game, is playable, and the game's own saving
  round-trips to the file. The run is `tools/play -Sav ""`, which passes
  `--config re/config/frontend.toml` - the cartridge is declared **512 KiB
  flash** instead of the runner's historical 8 KiB EEPROM fallback.
  `banks/save-demo/runner2.err.log` records the hand-driven session:
  `[save] cartridge backup: flash, 524288 bytes`, `[save] loaded 524288 bytes
  from ...\banks\save-demo\black.sav`, then 45 `[save] wrote 524288 bytes to
  ...\banks\save-demo\black.sav` flushes as the game saved in play. This is the
  **save** half of M3's "Done when" clause.
- **Observation (NEW GAME reaches gameplay on that configuration).** With the
  flash declared but **no save file attached**
  (`banks/runner-patched/nds_runner.exe --boot direct --generated-firmware
  --freebios --no-save --config re/config/frontend.toml --serve`), NEW GAME
  proceeds into the game. `banks/save-demo/run-flash-blank.json` (driven
  2026-10-18 23:33-23:38) presses A four times, the last labelled
  `confirm-new-game` at t=110.699 s, then keeps pressing on its own cadence:
  t=104.051-200.159 s, 206 samples, 137 distinct combined frames,
  `halt_stop: null`, and the stop is the probe's own `--stop-at 200 s`.
  `banks/save-demo/shots-flash/t00199.64s-A.png` is the in-game
  name-confirmation text box ("So your name's AAAA. What a wonderful name!"),
  so this is the first NEW GAME path that reaches gameplay.
- **Observation (the probe's `park` is idle, not the halt).**
  `park_loop_first_s` 110.699 / `park_loop_first_pc` `0x020882D8` / 143 parked
  samples is not a terminal stop: `0x020882D8` is an SDK idle address the very
  first smoke run already saw at vblank9=120 (above), the picture keeps
  changing, and the probe's own fatal-halt signature also requires a frozen
  picture (`check_stall`).
- **Observation (the plain save-less default is the one that crashes).** The
  historical `tools/play` invocation - `--no-save` with **no** `--config`, so the
  runner's 8 KiB EEPROM fallback - is the configuration in which NEW GAME still
  crashes at the sync-facade assert (`banks/HANDOFF.md`). Those drives predate
  the patch and were not re-run on the patched banks, so whether the crash
  survives the patch on its own is open.
- **Observation (hand-driven, maintainer).** A NEW GAME session played by hand
  on the save-capable configuration reached the starter choice and entered
  trainer battles - the first hand-confirmed gameplay in the port, and it is
  that same configuration `tools/play` now uses by default (the ROM's own
  `<rom>.sav` loaded, `re/config/frontend.toml` always passed). The session was
  not captured; the probe runs above are the reproducible evidence.
- **Unknown.** Which of the two changes present in every passing run above is
  load-bearing for NEW GAME - the `0x02075F7C skip_call` patch or the cartridge
  declaration - is not isolated. The splitting probe is the patched runner with
  `--no-save` and **no** `--config`: a crash there means the EEPROM default is
  the missing piece, a pass means the patch alone was enough.
- **Confidence.** High that the save-loaded run is playable (hand-driven, with
  the runner log) and that NEW GAME reaches gameplay with the flash declared
  (probe report + screenshots); medium on what inside that configuration is
  load-bearing. Windows only; the save-loaded run itself was not captured as a
  probe.

### The ARM7 alias hang is a missing live-byte validation (2026-10-19) — resolved

Closes the *Unknown (the mechanism)* of *The ARM7 alias banks are what stop the
boot*. The two `[[alias]]` blocks are enabled again in `re/config/black.toml`.

- **Observation (an alias range is not one module).** The coverage ingest that
  proposed the two ARM7 aliases (`banks/coverage-ingest/ingest-report.json`)
  reports the copy spans as *mostly* module content: it counts **17 page
  versions whose bytes are not in the ROM** among the pages the spans cover, and
  the `0x037F8000` span ends on the ARM7 WRAM pages (`0x03800000` among them).
  The bytes the guest holds at those addresses are materialized at run time, so
  the module's bytes are not what is there.
- **Observation (the dispatch contract makes that fatal).** A dispatch row
  emitted without validation carries a null validation pointer and is selected
  on its address alone - rank 0, which
  `runner/src/dispatch_lookup.h` documents as proving nothing about the bytes
  currently at `pc`. So the alias bank ran module-derived bodies at addresses
  where the guest held something else, which is the hang: `pc9=0x01FF8010`,
  `vblank9=0`, ARM7 parked at `0x00001078`.
- **Observation (the submodule's own precedent).** `PLAN.md` A1/A2 model exactly
  this case: relocated ARM7/ARM9 code that is materialized at run time belongs in
  a **content-validated** bank. `--validate-live-bytes` is the recompiler flag
  that emits `&g_validation_<bank>__N` per row plus the `NdsStaticValidation`
  tables, and `runtime_arm.cpp` gates each row on
  `validation_identity_live`, i.e. `bus_range_has_write_provenance` +
  `bus_live_bytes_equal` (`bus.cpp`).
- **Observation (the rule).** `pkmn_bankgen` now appends `--validate-live-bytes`
  to the recompiler invocation of every bank that declares at least one alias
  (`tools/bankgen/main.cpp`; the same flag is printed by the `--emit-only`
  hint). It is a consequence of the bank's own content, not a per-title switch.
- **Observation (the emission is byte-accurate).** All **187** validation
  structs emitted for `banks/black/bin/irbo_arm7_main.bin` compare equal to the
  image at the mapped source offsets (`0x02380000`-relative); 0 mismatches
  (`banks/scratch/alias_val_check.py`).
- **Observation (coverage).** With the aliases validated, `pkmn_bankgen
  --emit-only --title-config re/config/black.toml` gives arm9 **0** code_copy /
  937 seeds and arm7 **2** code_copy / 1387 seeds / 0 dropped. The recompiled
  arm7 bank is **3567 functions** (arm 3287 / thumb 280) in 8 body shards, and
  the flag also changes body codegen - the log reads `45 BL sites expanded`.
- **Observation (the boot is healthy again).** `tools/play -NoSave -Frames 280
  -Stats` reports `frame_fnv=(f462d3ff20fd83f4,06be2b0d14dc4383)` - the
  known-good pair - in every run so far (plain, `-Coverage`, and the four
  `-Diagnostics` bench runs). The historical hang signature was
  `(f5c5169d40c04383,f5c5169d40c04383)` with `vblank9=0`.
- **Observation (the Tier-3 load collapses).** `tools/play -NoSave -Frames 280
  -Stats -Coverage -Diagnostics` writes a manifest and a performance log per
  run; `banks/scratch/bench.ps1` files them under `banks/bench/<tag>/`. The
  interleaved pair below is the same 280-frame boot window, same runner, the
  only difference being the ARM7 bank (`f462d3ff20fd83f4,06be2b0d14dc4383` in
  all four runs, so the guest is bit-identical):

| counter | `a*-baseline49` (49 fns) | `b*-validated` (3567 fns) |
| --- | --- | --- |
| `tier3_entries7` | 1,407,701 | **1,550** |
| `tier3_insns7` | 35,154,815 | **24,813** |
| `tier3_entries9` | 22,978 | 23,003 |
| `tier3_insns9` | 684,389 | 684,389 |
| ARM7 `cache_hit_absent` | 166.6K-204.2K / frame | 104-113 / frame |
| ARM7 `tier3_arm7` (emu_attrib) | 23.4-29.5 ms/frame | **0.029-0.032 ms/frame** |

  The ARM9 halves match to within noise, so the ARM7 column is the effect. The
  alias code the ARM7 really runs (`a7=0x03806C14`/`0x03806C48` are the hot PCs)
  is now executed natively instead of interpreted.
- **Observation (it is a real win, measured A/B/A/B).** Emulated time for the
  same 280 frames: baseline **8.480 s / 8.348 s**, validated **5.624 s /
  5.568 s** - **-33.5%** (mean 8.414 s -> 5.596 s, i.e. 30.0 -> 20.0 ms/frame).
  Presented rate 30.5/31.1 -> 41.7/41.3 fps. Audio stops starving: underruns
  **220/202 -> 0/0** and `min_queue_frames` 7 -> 5026, and `slow32ms` 116/115 ->
  10/9. The `frame_fnv` pair is identical in all four runs. *(An earlier draft of
  this section compared the 280-frame window against the 250 M-cycle 2026-10-16
  manifest; that was apples-to-oranges and is corrected by this table.)*
- **Inference (high).** The hang was never a wrong CFG walk or a mis-keyed
  function: it was a *rank-0* dispatch row, selected on address alone, claiming
  bytes it had no right to claim. Content validation is what makes an alias bank
  sound, and it is the submodule's documented answer for relocated code.
- **Observation (what the ARM7 costs now).** With interpretation gone, the ARM7
  is the *second* cost, not the first: `exec_arm7` 8.79-9.11 ms/frame against
  `exec_arm9` 6.71-6.98 ms, `sched_other` ~2.3-2.7 ms, `tier3_arm9` 0.38-0.40 ms,
  `raster2d` 0.37-0.40 ms. Inside `exec_arm7` the dispatch tail dominates:
  `literal_fallthrough` ~44.3K events/frame, `literal_branch` ~14.2K,
  `resume_dispatch` ~5.0K. *(Caveat: `emu_attrib`'s `exec_*` and `sched_*`
  buckets are per-scheduler-round sampled estimates, so they only compare
  *within* one run. The buckets the log marks `exact` - `tier3_*`, `raster2d`,
  `dma_*` - are the ones that compare across the A/B, and they are the ones the
  win is read from.)*
- **Observation (`--coalesce-fallthroughs` has nothing to work with here).** The
  pass that folds a body's fall-through into a local `goto` is gated in
  `recompiler/src/main.cpp:build_superblocks` on `source == addr`, i.e. it skips
  exactly the relocated rows an alias bank is made of. Run by hand on
  `irbo_arm7_main` it reports `coalesced 40 fallthrough edges` - the 49
  image-resident functions - against 2769 address-adjacent same-mode
  function pairs in the bank, none of which cross a 4 KiB page. So the idea is
  dead for this bank until the pass accepts relocated rows (upstream).
- **Inference (medium).** Coverage and CPU cost are now separate problems: what
  is left on the ARM7 is *host-side dispatch overhead on statically recompiled
  code*, not interpretation. That is runner machinery, not bank content.
- **Unknown.** What bounds gameplay (the 280-frame splash is a pathological
  window - the ARM7 there is spinning in a poll loop with a control transfer
  every ~8 guest cycles), and whether an equivalent alias treatment of the ARM9
  ITCM span pays the same way.
- **Confidence.** High on the mechanism, on the boot, and on the A/B: identical
  digests, two interleaved pairs, and a 900-1400x Tier-3 drop. Medium on how the
  numbers transfer to gameplay. Windows, RelWithDebInfo, one ROM.

### The ARM7 aliases generalise to White, Black 2 and White 2 (2026-10-19)

Carries the resolved ARM7 fix from `re/config/black.toml` to the other three
titles. `re/config/{white,black2,white2}.toml` are new; the two ARM7 seed files
that Black staged move to `re/config/arm7/seeds/` and are shared.

- **Observation (the module is the same bytes in all four titles).**
  `pkmn_bankgen` prints the decoded ARM7 image SHA-1 for every ROM; all four
  report `364259352fffdcd15b75c2d9e997d2ed0d877190`, `raw 0x28F84 bytes`,
  `load=0x02380000` (`banks/bankgen-{black,white,black2,white2}.log`). So the
  relocated copies and the entry points observed inside them are a property of
  the module, not of a title, and the two `[[alias]]` blocks plus the 1387 ARM7
  seeds transfer verbatim. `re/config/arm7/seeds/` is where they now live;
  `re/config/black.toml`, `white.toml`, `black2.toml` and `white2.toml` all
  reference it, and each config notes that only its ARM9 half is title-specific.
- **Observation (the aliases alone are not the win).** A probe with the two
  `[[alias]]` blocks and *no* seeds gives `functions=236 (arm=234 thumb=2)` for
  White's ARM7; adding the 1387 shared seeds gives `functions=3567 (arm=3287
  thumb=280)`, exactly Black's bank. The seeds carry almost all of it — the
  finder's walk over the relocated range only finds what the module's own flow
  reaches, and the observed entry points are what reach the rest.
- **Observation (per-title emission).** `pkmn_bankgen <rom> banks/<title>
  --title-config re/config/<title>.toml` for each title reports arm9 `0
  code_copy, 0 seed(s)` and arm7 `2 code_copy, 1387 seed(s), 0 dropped outside
  image`; the ARM9 recompiles are unchanged (5265 / 5865 / 5865 functions, no
  seeds), and every ARM7 bank is recompiled with `--validate-live-bytes`
  (`banks/bankgen-{white,black2,white2}-generalise.log`).
- **Observation (the four ARM7 banks are the same bank).** Normalising the bank
  name out of the generated ARM7 sources, the 8 body shards, the dispatch and
  the header hash identically for all four titles (e.g.
  `irao_arm7_main_00.c` == `irbo_arm7_main_00.c` == `ireo_arm7_main_00.c` ==
  `irdo_arm7_main_00.c`, and likewise for every shard). The win is Black's win,
  replicated.
- **Observation (the boots are healthy and White's guest is bit-identical).**
  `tools/play -NoSave -Frames 1200 -Stats -Coverage -Diagnostics`. White before
  the bank change and White after present the same digest
  `frame_fnv=(749d014056024140,02da4c025df80383)`; Black 2 and White 2 present
  `(957b279301c98b19,02da4c025df80383)`. No hang, no zero-`vblank9`.
- **Observation (White's window speeds up; the ARM7 interpreter cost is gone).**
  Same 1200-frame window, same flags, the only change being the ARM7 bank:

  | counter | White, 49-fn ARM7 | White, 3567-fn ARM7 |
  | --- | --- | --- |
  | emulated time | 31.786 s (26.5 ms/frame) | **26.949 s (22.5 ms/frame)** |
  | presented rate | 32.72 fps | 36.51 fps |
  | `slow32ms` | 473 | **348** |
  | audio underruns | 1687 | **1297** |

  The saved 4.8 s is the ARM7 alias window (`tier3_arm7` after the change is
  0.093-0.095 ms/frame on all three titles, against the 23.4-29.5 ms/frame it
  cost Black before the fix); it is averaged over 1200 frames, most of which are
  past the boot window that does the interpreting.
- **Observation (what is left is the ARM9).** Over the same ~1180-frame samples
  the three titles show `tier3_arm9` **20.9-22.0 ms/frame** and `tier3_insns9`
  **120-121 M**, against Black's `tier3_arm9` 0.40 ms/frame. Their ARM9 banks
  are finder-only; Black's carries the 937 Tier-3 seeds, and that is the whole
  difference. So the ARM7 half of the interpreter cost is now closed on all four
  titles, and the ARM9 half is open on three of them.
- **Inference (high).** The generalisation is exact: identical image bytes,
  identical config, byte-identical emitted bank, same digest. Nothing about it
  is title-specific, and the shared seed directory is the honest expression of
  that.
- **Unknown.** How the three titles behave once their ARM9 is seeded (Black
  needed a `skip_call` patch at `0x02075F7C` for a `lock_leave_raw` assert;
  whether White / Black 2 / White 2 hit the same assert is not yet known), and
  what the numbers do over a gameplay window rather than a boot.
- **Confidence.** High on the bank identity and the boots; medium on the
  speed-up transfer, since only White has an interleaved before/after and only at
  one window length. Windows, RelWithDebInfo.

