# Plan.md

Roadmap for **pkmng5pc** — a native PC (Windows/Linux) reimplementation of
Pokémon Black/White and Black 2/White 2, driven by user-supplied ROMs.

This file is the source of truth for *what we build, in what order, and why*.
It intentionally separates **committed** milestones from **open** decisions.
Items marked **(open)** must be proposed before implementation (see `AGENTS.md`).

---

## 0. Problem statement

Four Gen 5 ROMs exist:

- Black / White (NDSi Enhanced, 256 MiB) — "BW"
- Black 2 / White 2 (NDSi Enhanced, 512 MiB) — "B2W2"

They share a Nitro/NitroFS base and much of the Game Freak engine, but differ in
overlays, data, and content. We want **one PC program** that:

1. Loads any of the four ROMs the user supplies.
2. Plays them with correct behavior (overworld, battles, menus, save).
3. Presents both generations from **one launcher**, with cross-version content
   behind a setting.
4. Ships **no copyrighted content**.
5. Later, adds modern rendering and New Game+ features without rewriting game
   logic.

This is a multi-year effort. The plan below front-loads de-risking: prove the
toolchain and a hardware abstraction layer on a vertical slice before
committing to a full-game strategy.

---

## 1. Hard constraints

- No ROM, save, or extracted asset is ever committed (`AGENTS.md` §2).
- Enhancement features (16:9, shadows, 60 fps, NG+) are **out of scope** until
  the core port milestone (M6) is met.
- Game logic never touches the GPU/OS directly; all hardware goes through the
  HAL.

---

## 2. Strategy (the central open decision)

There are three viable ways to get Gen 5 running natively. They are not equally
cheap, and the right choice depends on what the RE phase finds.

| Option | How | Pros | Cons |
| --- | --- | --- | --- |
| **A. Matching decompilation** | Find/adopt an existing pret-style decomp of BW/B2W2, compile it natively, implement platform layer | Highest fidelity, maintainable, easy modern rendering | Depends on a decomp existing and matching; large up-front |
| **B. Static recompilation** | Recompile ARM9/ARM7 machine code to C/LLVM, reimplement hardware | No decomp needed; byte-exact logic | Complex recompilation, hard to read/modify, HW fidelity work |
| **C. Engine reimplementation** | New engine reads the ROM's data files and reproduces logic | Full control, clean code | Reproducing an entire commercial engine by hand is enormous |

**Decision (D1, resolved 2026-10-14): strategy B — static recompilation.**

Evidence: no complete Gen 5 decompilation exists. Gen 1–4 decomps are mature
(pret), but Gen 5 is early: `pret/pokeblack`, `squiddonaut/pokeblack`,
`pokemodding/pokeblack` are partial disassembly efforts and `PlatinumMaster/
pokewhite2` "does not build any executables." By contrast, DS static
recompilation is proven feasible (`mstan/ndsrecomp` recompiles ARM9/ARM7 to C
and reaches the firmware main menu; `AetiasHax/ds-decomp` provides delink/
Ghidra/objdiff tooling). Therefore we recompile the games' ARM9/overlay/ARM7
code to C++ and implement the hardware as a HAL.

- **Reuse over reinvention:** adopt an existing NDS static recompiler as the
  recompiler + runner rather than writing one from scratch. **Decided (D7):** adopt
  `RetroPortingToolKit/ndsrecomp` pinned at tag `v0.6.10` **as-is**, keeping its
  melonDS-derived 3D GE (software + compute) and Wi-Fi device models, and
  license this project **GPL-3.0-or-later** to match. (`main` is PolyForm
  Noncommercial — unusable; the melonDS subtrees are GPL-3.0, the fork's own
  code at `v0.6.10` is MIT and GPL-compatible.) Recorded in
  `re/notes/recompilation.md`.
- **Method family:** this project is the **static-recompilation** family — the
  same as **Sonic Unleashed Recompiled** and **Zelda 64: Recompiled**
  (`N64Recomp`), *not* the decompilation-port family of **Ship of Harkinian**.
  A decomp port would need a full matching Gen 5 decompilation, which does not
  exist; a Ship-of-Harkinian-style path is only reachable later via the HLE
  layer.
- **Not an emulator:** we do not interpret/emulate the game at runtime. Game
  code is statically recompiled and linked natively; only hardware is
  reimplemented (HAL). There is no shipped emulator core. The unavoidable
  "runtime" that services memory-mapped hardware registers is a port, not an
  emulator.
- Revisit strategy **A** only if a matching Gen 5 decomp matures; the recompiled
  code and HAL would remain reusable.
- Option **C** alone is not planned.

Regardless of choice, the **HAL + renderer** work is shared.

---

## 3. Milestones

Legend: `[ ]` pending, `[~]` in progress, `[x]` done. Each milestone lists
**acceptance criteria** (definition of done).

Ordering after M6: M7, M8 (+M8.5), M9, M11, then M10 (far future).

### M0 — Foundation & RE toolchain — **status: done**

- [x] Initialize git repo, `.gitignore` (ROMs, saves, extracted, build, idb).
      (Repo initialized; commits are left to the maintainer.)
- [x] CMake skeleton + `CMakePresets.json` for `windows-*` and `linux-*`.
- [x] CI: build + `ctest` on both OSes (asset-free fixtures only).
- [x] `.clang-format` / `.editorconfig` / license (**GPL-3.0-or-later**) +
      `NOTICE` stating the program ships without ROM content.
- [x] REA/Ghidra note templates under `re/notes/`.

**Done when:** `cmake --preset linux-debug && ctest` passes on an empty tree
with a placeholder test. — **met** (verified locally on `windows-debug`; CI
covers Ubuntu and Windows).

### M1 — ROM & asset extraction pipeline (`tools/`) — **status: in progress**

- [x] `romdump`: parse NDS header (title, game code, ARM9/ARM7 offsets/sizes/
      entries, overlay tables, FNT/FAT) for all four ROMs.
- [x] `romdump --extract <dir>`: write the full inventory + a SHA-256 manifest
      (`manifest.sha256`, `sha256sum` format). Verified deterministic across
      runs on Black (54539 files, 0 skipped).
- [x] NitroFS: walk FNT/FAT (directory main-table + sub-tables), list files.
- [x] NitroFS: extract by path into `extracted/` (`nitrofs/<path>`).
- [x] Decompressors: LZ10 (`0x10`), LZ11 (`0x11`), Huffman4 (`0x24`),
      Huffman8 (`0x28`), RLE (`0x30`) — round-trip unit-tested.
- [x] BLZ ("bottom LZ"): decompress ARM9 and ARM9 overlays. All 229 compressed
      BW overlays and the ARM9 binary decode to their exact declared sizes.
- [ ] Decompressor: Huffman `0x20` variant (low priority; not observed yet).
- [x] NARC: parse header/`BTAF`/`BTNF`/`GMIF`, enumerate members (Black: 237
      archives, 54054 members; empty-BTNF and named-BTNF both handled).
- [x] NARC: extract members into `extracted/` (`narc/<archive>/<id>`, nested
      containers recursed as `<id>.narc` + `<id>.d/`).
- [x] Overlays: read per-ROM overlay table, decompress, verify size vs RAM.
- [x] Overlays: dump decoded `ovNNN.bin` into `extracted/`.
- [x] SHA-256 (`pkmn::sha256`): portable, known-answer tested; used for the
      extraction manifest.
- [ ] Asset parsers: NCGR/NCLR/NSCR/NCER/NANR (2D), NSBMD/NSBTX/NSBMA/NSBTA
      (3D), SDAT→SSEQ/SBNK/SWAR/SWAV/STRM (audio) (not started).
- [ ] Identify NARC member formats (sniff magic: `RGCN`/`RLCN`/`BMD0`/`SDAT`/…).
- [x] ROM-format schemas documented in `re/notes/header.md`,
      `re/notes/overlays.md`, and `re/notes/nitrofs.md`.

**Next up (M1 remainder):** identify NARC member formats (sniff member magic),
then implement the 2D/3D/audio asset parsers on top of the extracted tree. This
unblocks M2's asset/decomp investigation.

**Done when:** a fresh checkout, pointed at a user ROM path, produces a complete
`extracted/` inventory whose SHA-256 manifest matches across repeated runs, and
`tests/` covers every decompressor round-trip.

### M2 — Faithful boot to the main menu (static recompilation)

The goal of M2 is the real thing: run the game's own code (statically
recompiled) against the HAL until **Black's title screen → press Start → main
menu** appears. Target: push through to the 2D logo/menu visuals (M2.3+);
Windows and Linux are developed in lockstep. BIOS is flexible (HLE preferred,
user-supplied BIOS optional). Logos are 2D, so a visible win precedes the 3D GE.

**Bring-up plan for the first visible screen (agreed 2026-10-16).** The
deliverable is a screen you can see, and the work is ordered to remove false
constraints first:

- **Target:** Black only, through M2.3 (2D logo/title visuals) into M2.5
  (title → Start → main menu). Overworld rendering stays in M3. *Widened
  2026-10-16:* all four titles are now banked and reach the intro and 3D title
  screen; only Black has been driven past the title.
- **Gate = pixels, not fps.** The gate is a *correct frame*, proven from the
  runner's own debug server (`--serve`, `run_to_event`, `framebuffer A|B`), not
  a subjective look at a window. Throughput is a separate workstream.
- **Interpreter coverage is part of the gate.** Tier 3 remains the correctness
  floor, but must serve **< 0.1 %** of executed guest instructions in play, read
  from the coverage manifest — `docs/adr/0003-tier3-interpreter-policy.md`.
- **Bring-up runs use an optimized build.** Every earlier throughput number came
  from `CMAKE_BUILD_TYPE=Debug` and is void until re-measured.
- **Overlays are banked ahead of time.** Game logic lives in `ovNNN`; the
  runner's live-overlay path (run-time recompile + `dlopen`) is rejected for the
  shipped configuration. Scope starts with the overlays the boot path loads.
- **The recompiler sees what we declare.** Where code is copied at run time
  (ARM7 → WRAM/main RAM), declare it with `[[code_copy]]` seeds derived from
  evidence rather than accepting Tier 3.
- **Everything reproducible lives in our tree.** Checked-in per-title configs
  under `re/config/` (addresses and reasoning, no ROM bytes, no ROM-derived
  hashes); generated `banks/` stay ignored; `third_party/ndsrecomp` stays
  pristine and pinned at `v0.6.10`.
- **Boot path:** `--boot direct --generated-firmware --freebios` (no dumps
  required). Revisit only if game behaviour proves to need firmware state.
- **Visual evidence is hashes and counters** in `re/evidence/`, never images.

##### M2.0 — Strategy + RE setup

- [x] Evaluate decomp availability; resolve D1 as **B** (§2).
- [~] Stand up static analysis: `objdump` entry/crt0 pass done (`arm9-init.md`);
      Ghidra project for `arm9.bin` (base `0x02004000`) + overlays still to do;
      symbol map seeded at `re/symbols/black-arm9.json`.
- [x] Locate ARM9 entry (`0x02004800`), crt0, and the THUMB SDK main
      (`0x02005124`); recorded in `re/notes/arm9-init.md` with Evidence IDs.
- [x] Select the recompiler + runner (D7): adopt `ndsrecomp` @ `v0.6.10`
      **as-is** (melonDS 3D GE + Wi-Fi retained); license this project
      **GPL-3.0-or-later**. Method = the recomp family (Sonic Unleashed
      Recompiled / Zelda64Recomp). Recorded in `re/notes/recompilation.md`.

##### M2.1 — Execution substrate (headless)

- [~] Recompile ARM9 + overlays (and the ARM7 image) to C++ with a runner that
      keeps original RAM addresses; live-code fallback path for undecoded/mutable
      code. **ARM9 + ARM7 static banks now generate, link, and are wired into the
      build** (`pkmn_bankgen` → `banks/<title>/`, `PKMNG5PC_BUILD_RUNNER`); see
      `re/notes/recompilation.md`. Overlays are deliberately left to the runner's
      live-overlay path. Discovery is uneven: the ARM9 bank holds 5265 functions
      without seeds (7175 with Black's 937 Tier-3 seeds), and the ARM7's 49
      discovered functions become **3567** once its two relocated copies and the
      shared Tier-3 seeds are declared. All four titles now use that ARM7 bank,
      so the ARM7 no longer runs mostly interpreted; the ARM9 still does on the
      three titles with no per-title ingest.
- [x] Memory bus, CP15/TCM, WRAM, IRQ + timers, DMA, IPC/FIFO, SPI/firmware
      defaults; a **reimplemented BIOS** (FreeBIOS, `--freebios`) so no retail
      BIOS dump is required. Supplied by the adopted runner; now exercised by a
      real boot of Black (`re/notes/recompilation.md`).
- [x] **Gate:** ARM9 runs crt0 → `NitroMain` with correct register/RAM state.
      Observed: the ARM9 executes past `NitroMain` (`0x02005124`) into NitroSDK
      code at `0x02079xxx`, and the game programs CP15 DTCM itself.

##### M2.2 — Filesystem + OS (LLE, not HLE)

The adopted runner does **not** HLE the NitroSDK. It refuses "load-bearing HLE"
(`ndsrecomp` `docs/dispatch_architecture.md`), recompiles the game's *own*
NitroSDK code, and emulates the cartridge interface instead. There is no FS
layer to write: the game's file calls are recompiled code talking to an
emulated cart. Our `rom/` layer is therefore an **offline** library (bank
generation, M6 save I/O, M7 launcher detection), not a runtime component.

- [x] Confirm the emulated cart path serves the game's own reads; no HLE seam.
      **Met 2026-10-18:** a full NEW GAME session loads and uses the game's own
      archives — text, fonts, maps, scripts, Pokémon data — and every file read
      in it is the recompiled game's own call.
- [x] **Gate:** the game loads its own data files (e.g. `gfl_font.dat`,
      `skb.narc`). **Met 2026-10-18** with the same session: the game draws text
      in its own fonts and its message boxes read the game's own text. Evidence:
      `re/notes/recompilation.md` §"Save-loaded runs are playable; NEW GAME
      needs the save-capable build".

##### M2.3 — Video HLE + renderer (first M2 visual target)

- [~] Map VRAM/PAL/OAM/BG registers; implement the DS 2D compositor
      (text/affine/bitmap BG, OBJ, windows, blending, master brightness).
      Supplied by the adopted runner, which documents 2D as pixel-exact on the
      firmware paths it exercises and does implement windows (its own
      `ISSUES.md` listing them as unimplemented is stale). Display
      FIFO/capture is genuinely missing.
- [x] Bring up the SDL3 window + render backend (both OSes) and present both
      256×192 screens. CMake fetches SDL3 (3.4.18, static) and the runner builds
      and runs against it; `--interactive` opens the window and `tools/play`
      wraps the invocation. **Met 2026-10-16:** the live window displays the
      boot splash (top screen content, bottom blank) at ~45–50 presented
      frames/s. Pacing is the audio queue, so the port is live but not 1:1.
- [x] GE (`GXFIFO`) stubbed initially; enough later for the title scene. **Met
      2026-10-18** — and well past it: the intro cinematic, the outdoor town in
      gameplay and the battle scenes are 3D, on the live window and in the probe
      frames.
- [x] **Gate:** boot splash / logos render. **Met 2026-10-16** (top screen;
      the bottom screen is correctly blank at this point in the sequence).
      Evidence: engine A enables **BG1** at `vblank9=240` (~4 s of emulated
      time) and the captured frame shows the Pokémon Company + Nintendo
      splash; frame hashes and the PPM/PNG are under git-ignored `banks/`,
      the method and hashes are in `re/notes/recompilation.md`. The previous
      "measured negative" was a **near miss**: it sampled at 223 vblanks.
      Also confirmed through the **live window**, not only the debug server.
      **Widened 2026-10-16: the entire intro cinematic renders** — Game Freak
      logo, castle/throne, Zekrom, cast, "You and Your Pokémon!" card, 146
      distinct frames — so the gate is met well past the splash. The run then
      stops at t≈83.69 s, where the scene load begins; that stop is a vendored
      `gpu2d` worker-pool defect, worked around for all runs with
      `NDS_GPU2D_THREADED=0` (see the M2.5 note), not a coverage or guest
      problem.

##### M2.4 — Input, timing, audio

- [~] Scanline/vblank + IRQ timing; key input + touch through the HAL. Key
      input is exercised end-to-end through the frontend's own handler (real SDL
      events → HAL button state) by the M2.5 gate below — and since then by a
      full gameplay session (walking, menus, battles); the vblank-driven path
      produces every frame above. **Touch is still unverified.**
- [ ] SDAT/SSEQ playback via the ARM7 sound path.
- [~] **Gate:** intro/title runs with input and music. **Partially met
      2026-10-18:** input drives the whole run end-to-end; **music/SFX are still
      unverified by ear** (SDAT/SSEQ is audio-structural only so far).

*Status 2026-10-18:* key input is exercised end-to-end through the frontend's own
handler (real SDL events → HAL button state) by the M2.5 gate below, and now by
a full gameplay session; the box stays open for **touch** and for **audio**.

##### M2.5 — Reach the main menu

- [x] Iterate remaining subsystems until title → Start → menu text renders.
      **Met 2026-10-16.** The title takes an **A** press (Start does not) and the
      four-entry startup menu renders: NEW GAME (selection bar), MYSTERY GIFT,
      NINTENDO WFC SETTINGS (wireless icon), MIC TEST — over the trainer art, with
      legible text and engine B blank; no CONTINUE entry under `--no-save`.
      Evidence: `re/notes/recompilation.md` §"M2.5 gate".
- [x] **Gate:** **Black's main menu displays and is navigable. This is the first
      real de-risking gate.** **Met 2026-10-16, on a live window.** The selection
      bar walks NEW GAME → MYSTERY GIFT → NINTENDO WFC SETTINGS → MYSTERY GIFT
      under Down/Down/Up, and A opens MYSTERY GIFT's own menu (RECEIVE GIFT /
      CHECK THE CARD ALBUM / INFO / QUIT) with "Welcome to Mystery Gift!" on the
      bottom screen, whose cursor then moves too. Keys were injected into the live
      frontend's real input path (`banks/probe_live.py`; montages
      `banks/shots-live/`).
- [x] **All four titles boot** *(added 2026-10-16)*. One runner build registers a
      bank group per ROM SHA-1; White, Black 2 and White 2 were played on a live
      window through the intro cinematic to the **3D title screen** — the same
      sequence Black shows (the test was exactly that: play the intro, watch the
      title). No per-title work was needed: `bankgen` on the ROM was enough.
      Evidence: `re/notes/recompilation.md` §"All four titles boot to the intro
      and the 3D title screen".

**Done when:** Black boots from the user's ROM to its navigable main menu on
Windows and Linux, entirely through recompiled game code + the HAL (no emulator
core, no direct OS/GPU calls from game code). **Windows: met 2026-10-16**, on a
live `--interactive` window; the Linux side is unverified. **All four titles met
the weaker "boots to the 3D title screen" bar the same day** — the roadmap's
M7 goal of all four running now has its first foothold (intro + title), with the
menu/overworld still Black-only.

### M3 — Overworld vertical slice

- [x] Map system: matrix + headers + area data + object/event tables loaded from
      ROM (`re/notes/map-system.md`). **Met 2026-10-18** through the recompiled
      game's own loader: the room and Nuvema Town render with their object sets
      and change on area transitions.
- [x] DS 2D engine: BG layers (NSCR+NCGR+NCLR) and OBJ sprites composited.
      **Met 2026-10-18:** tile layers and sprites (player, NPCs, menus, message
      boxes) composite on both screens.
- [x] DS 3D GE emulation: NSBMD/NSBTX draw for outdoor maps; camera. **Met
      2026-10-18:** the adopted GE renders the 3D title, the intro cinematic and
      the outdoor town.
- [x] Script VM: interpret at least movement/lock/release/warp commands
      (`re/notes/script-vm.md`). **Met 2026-10-18:** the game's own script VM
      drives the NEW GAME sequence — scripted movement, area transitions, the
      gift-box starter choice, dialogue gating.
- [x] Text system: render a message box from the ROM text archive
      (`re/notes/text-system.md`). **Met 2026-10-18:** in-game message boxes
      render from the ROM's text archive (Juniper's intro, the name prompt,
      in-game dialogue).

**Done when:** you can boot a save (or new game), render a town route, walk with
the D-pad, trigger a warp, and read a text box — all through the HAL.

*Status 2026-10-18:* **first met on Windows, by hand.** The maintainer played a
NEW GAME on the save-capable build (the ROM's own `<rom>.sav` loaded,
`re/config/frontend.toml` declaring the 512 KiB flash cartridge) into Nuvema
Town, walked with the D-pad, went through the game's own area transitions and
read its text boxes, then chose a starter and entered trainer battles (battles
are M4, not claimed here). The session was not captured frame-by-frame; its
reproducible counterpart is the `run-flash-blank` probe, which drives NEW GAME
from the menu to the in-game name-confirmation box (`halt_stop: null`,
`banks/save-demo/run-flash-blank.json`), while a hand-rolled run that leaves the
cartridge at the runner's 8 KiB EEPROM default (no `--config`) still crashes on
NEW GAME at the sync-facade halt (`banks/HANDOFF.md`). The five systems above are
ticked on the strength of this: they are the **recompiled game's own** code plus
the runner's HAL (the LLE stance of M2.2), not port-side re-implementations, so
the per-subsystem RE notes (`re/notes/map-system.md`, `script-vm.md`,
`text-system.md`) are still owed. Linux unverified. Evidence:
`re/notes/recompilation.md` §"Save-loaded runs are playable; NEW GAME needs the
save-capable build".

### M4 — Battle system

- [ ] Pokémon/move/item/trainer data loaders (`re/notes/pokemon-data.md`).
- [ ] Battle flow: wild + trainer battles, turn resolution, damage calc, AI.
- [ ] Battle presentation: sprites, HP bars, move FX (DS GE/2D).
- [ ] Audio: SSEQ playback for battle music, SFX via the ARM7 audio HAL.

**Done when:** a full wild and trainer battle can be fought to completion with
correct results and sound.

### M5 — Full game systems

- [ ] Menus (party, bag, Pokédex, PC, options, map).
- [ ] Overworld events, items, encounters, trainers, evolution, trades.
- [ ] Entralink / Driftveil / post-game hooks as reachable (best effort).
- [ ] B2W2-specific content paths and engine deltas.

**Done when:** a save can be played for a long session across maps with menus,
items, and battles working.

### M6 — Save compatibility (**core port complete**)

- [ ] Byte-exact `.sav`/`.dsv` read/write (0x24000, block rotation + checksums).
- [ ] Import existing save, play, export, reload in emulator/hardware.
- [ ] Golden round-trip tests (synthetic fixtures, no real save committed).

**Done when:** import → play → export produces a byte-identical save and the
in-game load accepts the exported file. **Enhancements may begin after this.**

### M7 — Launcher & multi-ROM

- [ ] Launcher UI: pick ROM, show detected title/game code.
- [ ] One binary drives all four games; load the right title profile per ROM.

**Done when:** all four ROMs launch and play from one menu.

### M8 — Modern rendering (post-M6)

- [ ] **Present/compositor:** our own layer that lays out and post-processes the
      two DS screens, replacing the runner's fixed 512×768 top-over-bottom
      present. One home for layout, aspect ratio, and post-FX. Owning this also
      means our own present loop, decoupled from the runner's frontend.
      *(The seam landed 2026-10-19: `render/backend.h`, `compose_screens()`, a
      Null and a shader-free Vulkan presenter — `docs/adr/0005-vulkan-first-renderer.md`.
      The compositor itself, and wiring it into the frontend, are what remain.)*
- [ ] **Layout:** dual-screen layout for arbitrary aspect ratios (stacked,
      side-by-side, ultrawide) with safe areas. Game logic unchanged.
- [ ] **Frame decoupling:** guest logic stays locked at 60 Hz (no speedup) while
      presentation is uncapped and paced independently of the audio queue;
      audio is resampled, not used as the clock (supersedes the current
      "present clock = audio queue"; a new ADR is owed).
- [ ] **3D resolution scaling:** render the DS GE above native resolution, with
      an explicit policy for the 2D layers (BG/OBJ/fonts) versus the upscaled 3D
      layer; a new ADR is owed.
- [ ] **Post-FX:** bloom on the composited frame (low risk).
- [ ] **Post-FX:** shadow maps for GE scenes. Risky — the DS exposes no
      depth/normal buffer we control. Gate on a prototype before committing.
- [ ] **Late:** interpolation above 60 Hz (synthesised in-between frames) and
      full uncap/speedup. Large and uncertain; recorded, not scheduled.

**Done when:** each effect is toggleable and off by default; enabling them does
not change game logic results (verified by save/frame comparisons).

### M8.5 — Single-screen UI mode (post-M8, large)

Rework battles and menus to play on **one screen**, in the style of the
Switch-era and older handheld titles. The dual-screen UI is baked into the
game's own screens, so this is a presentation-and-flow project, not a setting.
Explicitly late; not a dependency of M8.

**Done when:** a full play session — overworld, battles, menus — is usable on a
single screen with no information hidden behind the second display.

### M9 — New Game+ / New Game carry (post-M6)

- [ ] NG+ difficulty: restart a title with harder trainers/gym leaders while
      retaining the party.
- [ ] Cross-title carry ("New Game Continue"): a completed **Black** save starts
      a **harder Black 2 / White 2** run carrying the Black team — a
      continuation, not a fresh file.
- [ ] Carry survives the `.sav` format via a sidecar; never corrupt or rewrite
      the vanilla save.
- [ ] Difficulty scaling table (levels, AI, items) configurable.

The game's own **Unova Link / Memory Link** (native BW→B2W2 data transfer) is a
different feature and is out of scope here.

**Done when:** a completed Black save can start a harder same-title run or a
harder B2W2 run carrying the previous party.

### M11 — Nuzlocke mode (forced rules) — post-M6

- [ ] Define the **hook seam** (an explicit interception point over RE'd game
      functions) without editing any recompiled unit; record it as an ADR (a
      game-logic interception policy) that reconciles with `AGENTS.md` §8.
- [ ] Hook the encounter roll, faint/release, and in-battle item use to enforce
      first-encounter-only, faint = permanent, no items in battle, and the dupes
      clause (see D10 for which rules are mandatory vs toggleable).
- [ ] Mode flag plus **sidecar** state; the vanilla `.sav` stays byte-exact.
- [ ] Options surface in the launcher and/or in-game options.

**Done when:** a Nuzlocke run enforces its rules in-game, and the save file
written is byte-identical to what the unmodified game writes.

### M10 — Later host platforms: Android, then web (far future, not scheduled)

Recorded so that earlier decisions do not accidentally foreclose them. **Not a
commitment**, not a date, and not a reason to add abstraction now; it sits after
M8/M9 and after the core port (M6). The design constraints below are the part
that binds *today*.

**Android (first of the two, if either).** Game logic is already hardware-free
(§5), the runner is portable C++20 and SDL3 targets Android, so this is a
platform job rather than a rewrite: an activity/launcher that takes the user's
own ROM through the system file picker, plus touch/audio/window and a
Vulkan-or-GL backend behind `render/backend.h`. Things to weigh when the time
comes: the GPL-3.0 melonDS GE/Wi-Fi subtrees must keep compiling for the target
ABI, NDK packaging must not bundle ROM content, and save I/O must go through the
M6 byte-exact path on shared storage.

**Web (WebAssembly).** Same ROM-at-runtime posture: the page must never host,
fetch, or relay ROM content (`AGENTS.md` §2.5); the user selects their own ROM
locally. Three consequences worth keeping in mind *now*:

- **No runtime code generation is possible on web.** Emscripten cannot host the
  runner's live-overlay compiler path, and WASM has no JIT seam. This is a
  second, independent reason `docs/adr/0003-tier3-interpreter-policy.md` is
  load-bearing: ahead-of-time banks are the only execution strategy that can
  ever ship on web.
- Threads need `SharedArrayBuffer` (COOP/COEP), so the runner's dual-CPU
  scheduler and threaded compositor would need a pthreads or single-thread
  build, and the audio device a web backend.
- GPU: a WebGPU (or GL/WebGL2) backend behind the same `render/backend.h` seam;
  assume the DS GE path ships software-only first. GPL-3.0 plus WASM means
  serving the binary is distribution, so corresponding source must ship with it.

**Done when:** each platform boots a user-supplied ROM to a navigable main menu
from one launcher, with no ROM content in the artifact and no runtime
compilation.

---

## 4. Cross-cutting concerns

- **Determinism:** game logic must be frame-rate independent; enhancements read
  state, they don't mutate timing.
- **Fidelity harness:** record (state, input) → expected result fixtures so the
  port can be diffed against the emulated original.
- **Performance budget:** target 60 fps at 1080p baseline on mid-range hardware;
  profile the compositor early.
- **Localization:** text pulled from the ROM at runtime; UI strings from a small
  in-repo table (no ROM text committed).

---

## 5. Risks

| Risk | Impact | Mitigation |
| --- | --- | --- |
| No usable Gen 5 decomp | Strategy A collapses | **Resolved:** strategy B chosen (M2.0) |
| Recompiler coverage / undecoded or self-modifying code | Wrong behavior, crashes | Live-code fallback; trace diff vs reference during bring-up |
| Recompiler/bring-up tool license | Can't ship or vendor | **Resolved:** adopt `ndsrecomp` as-is under GPL-3.0-or-later (D7) |
| DS 3D GE fidelity (fixed-point, polygon quirks) | Visual/behavioral bugs | Unit-test GE math; compare against emulator frames |
| Scale of full-game logic | Years of work | Vertical slices; ship overworld before battle |
| Save format misread | Data loss for users | Golden round-trip tests; never ship without M6 |
| Legal exposure | Project death | `AGENTS.md` §2; ROM-at-runtime only |
| Enhancement regressions | Broken base game | Effects off by default; logic/visual separation |

---

## 6. Open decisions

- **D1:** Strategy A vs B vs hybrid (§2) — **resolved: B (static
  recompilation)**, 2026-10-14.
- **D2:** Renderer abstraction: hand-rolled RHI vs bgfx vs sokol.
- **D3:** C++20 (default) vs Rust for new isolated components.
- **D4:** Cross-version content toggle — **resolved: dropped** (2026-10-16). The
  launcher still drives all four ROMs; there is no per-title content swap.
- **D5:** Scope of B2W2 post-game/Entralink for the initial port.
- **D6:** Whether NG+ difficulty is data-only or adds new AI.
- **D7:** Recompiler source — **resolved: adopt `ndsrecomp` @ `v0.6.10` as-is
  (melonDS 3D GE + Wi-Fi retained), license this project GPL-3.0-or-later**
  (2026-10-14; supersedes the earlier "strip GPL, keep MIT" decision). Method
  family = static recompilation (option 1: faithful floor).
- **D8:** BIOS handling — **resolved: HLE SWIs via `freebios` (BSD-2) by
  default**; a user-supplied BIOS is optional, never shipped.
- **D9:** Later platforms (M10) — Android before web, or web first? Unresolved
  and deliberately so; nothing is scheduled until M6 is met.
- **D10:** Nuzlocke rule set — which rules are mandatory and which are toggleable
  (dupes clause, shiny clause, gift/static encounters, level cap)? Unresolved:
  the hook seam (M11) is decided, the rule manifest is not.

---

## 7. References (no content embedded)

- NDS hardware: GBATEK; melonDS/DeSmuME as behavioral references.
- Nitro formats: Nitro System docs, Tinke, lowlines' Nitro tooling.
- Gen 5 data: DSPRE (open source) as a format reference.
- Decomp ecosystem: pret org decomps (Gen 3–4); Gen 5 efforts
  (`pret/pokeblack`, `squiddonaut/pokeblack`, `PlatinumMaster/pokewhite2`) —
  early, not yet buildable.
- DS static recompilation: `mstan/ndsrecomp` (ARM9/ARM7 → C, SDL host),
  `Myth7x/ndsrecomp` (ROM → native C/CMake project), `AetiasHax/ds-decomp`
  (delink/Ghidra/objdiff tooling).

Do not vendor or copy their content; cite and reimplement.

---

## 8. Progress log

Newest first. Keep entries short; details and evidence live in `re/notes/`.

- **2026-10-19 (the ARM7 alias win is generalised to White, Black 2 and White 2)**
  — the four Gen 5 ARM7 module images are **byte-identical**
  (`36425935…`, `0x28F84`, load `0x02380000`), so the two relocated copies and
  their 1387 observed entry points are a property of the module, not a title.
  `re/config/{white,black2,white2}.toml` are new, the ARM7 seed files moved to a
  shared `re/config/arm7/seeds/`, and every title's ARM7 bank is now **3567
  functions**, byte-identical across the four titles modulo the bank name, each
  recompiled with `--validate-live-bytes`. The three ARM9 halves stay
  finder-only (5265 / 5865 / 5865). All three boot with the guest they had before
  (White `frame_fnv=(749d014056024140,02da4c025df80383)`, unchanged from the
  49-function bank; both B2W2 titles `(957b279301c98b19,02da4c025df80383)`), and
  White's 1200-frame window drops 31.786 → **26.949 s** emulated and 473 → **348**
  `slow32ms`. **Scope:** Windows, boot windows; the remaining interpreter cost is
  the ARM9 (`tier3_arm9` 21-22 ms/frame on the three titles, against 0.40 on
  Black). `re/notes/recompilation.md` §"The ARM7 aliases generalise to White,
  Black 2 and White 2".

- **2026-10-19 (the ARM7 alias work lands, and it is a 33.5% boot win)** — the
  two ARM7 `[[alias]]` blocks in `re/config/black.toml`, disabled since
  2026-10-16 because declaring them hung the boot, are enabled again with the
  root cause fixed: an *alias range is not uniformly module content* — it carries
  run-time-materialized pages (the coverage ingest itself counts 17 page versions
  "not in ROM"), and a dispatch row emitted without validation is selected on its
  address alone, so the bank ran module code where the guest held something else.
  `pkmn_bankgen` now recompiles any bank that declares an alias with the
  recompiler's `--validate-live-bytes`, so every row proves the bytes at the pc
  before it runs (`re/notes/recompilation.md` §"The ARM7 alias hang is a missing
  live-byte validation"). Result: the ARM7 bank goes 49 → **3567 functions**,
  Tier-3 ARM7 entries 1,407,701 → **1,550** over the same 280-frame window,
  emulated time 8.414 s → **5.596 s (-33.5%)**, presented rate 31 → 42 fps, and
  audio underruns **220 → 0**. The guest is bit-identical throughout
  (`frame_fnv=(f462d3ff20fd83f4,06be2b0d14dc4383)` in all four interleaved runs).
  **Scope:** Windows, Black only; the A/B is a boot window and the ARM9 ITCM span
  still interprets.

- **2026-10-19 (the renderer seam exists; Vulkan is the first backend)** —
  `render/` adds `pkmn::render::Presenter` behind `render/backend.h` — the single
  place allowed to name a GPU API — plus `compose_screens()` as the one home for
  dual-screen layout. The first backend is Vulkan and is deliberately
  shader-free (staging buffer → `vkCmdCopyBufferToImage` → present), so the seam
  can be built and exercised with no Vulkan SDK and no shader compiler; entry
  points are resolved at run time and a missing driver degrades to a Null
  presenter. `docs/adr/0005-vulkan-first-renderer.md` records the decision and the
  verification. **This is the seam, not M8:** GPU-side
  compositing, resolution policy, frame decoupling and post-FX remain open.

- **2026-10-18 (Black reaches gameplay — played by hand past the sync-facade
  halt)** — the NEW GAME transition used to park the ARM9 forever in the sync
  facade's own fatal-halt routine (`0x02088234`, halted at t=112.6 s emulated).
  Two bring-up facts fixed it, neither of them a change to game logic: the
  facade releases one resource twice, an environment-only mismatch, declared as
  a `skip_call` patch at `0x02075F7C`
  (`docs/adr/0004-declared-bank-patches.md`); and a Gen 5 cart is 512 KiB flash,
  not the runner's 8 KiB EEPROM fallback (`re/config/frontend.toml`). With both,
  NEW GAME runs through the intro into gameplay — the maintainer then played by
  hand: starter chosen, trainer battles entered. **`tools/play` now plays on the
  ROM's own `<rom>.sav` by default** and always passes the cartridge table
  (`-NoSave` / `--no-save` is a blank cart that writes nothing). Headless probe
  timings, the runner log and the frames: `re/notes/recompilation.md`
  §"Save-loaded runs are playable; NEW GAME needs the save-capable build".
  **M3's system boxes are ticked on the strength of it** (the milestone's "Done
  when" clause is met on Windows for the first time; Linux unverified).
  **Scope:** Windows, Black only (Linux unverified); the bank patch mechanism is
  new and only this one patch is declared.

- **2026-10-16 (all four titles boot — White, Black 2 and White 2 join Black at
  the 3D title screen)** — the three other ROMs were banked with plain
  `pkmn_bankgen` (no per-title config exists for them yet: no `[[alias]]`/seed
  work, 5265 / 5865 / 5865 ARM9 functions), and one `nds_runner` was rebuilt with
  the runner's multi-title form (`NDS_TITLE_BANK_DIRS` / `NDS_TITLE_ROM_SHA1S`),
  so a single binary registers a bank group per ROM SHA-1 and picks it at boot.
  Played by hand on the live window, each runs the intro cinematic and renders
  the **3D title screen** — the same visible sequence Black shows. **Scope:** the
  test was the intro and the title screen only; the A-press main menu stays
  Black-only, no per-title coverage manifest was read, and Linux is unverified.
  Bank counts, the four ROM SHA-1s and the B2W2 ARM9 shape (326 ARM / 5539 Thumb):
  `re/notes/recompilation.md` §"All four titles boot". This is M7's "all four
  ROMs launch and play from one menu" at its first step.

- **2026-10-16 (vision alignment: goals fixed, plan reshaped)** — sat down with
  the maintainer and wrote the target picture into the plan. **Platforms:**
  Windows/Linux → Android → web, and no other host targets are to be named.
  **Frame rate:** the current goal is a locked 60 Hz logic rate with a decoupled,
  uncapped present (no speedup); interpolation above 60 and full uncap are late
  goals, recorded but unscheduled. **Cross-version content toggle: dropped**
  (D4). **Cross-title carry** is now a concrete feature — a completed Black save
  starts a *harder* B2W2 run with that Black team ("New Game Continue"); the
  game's native Unova Link is explicitly a different, out-of-scope feature.
  **Single-screen play** is recorded as a large late goal (M8.5), not early.
  **Nuzlocke** is a new post-M6 milestone (M11) enforced through a **hook seam**
  over RE'd game functions, with the vanilla save kept byte-exact; the rule
  manifest is open (D10). **Post-FX** is split into bloom (low risk) and shadow
  maps (prototype-gated). The **modding suite and modding agent skills are out of
  scope** — a separate project. `AGENTS.md` §2 gains an **asset-provenance** rule:
  AI is development-only and nothing AI-generated ships. Two ADRs are still owed:
  game-logic interception (M11) and frame decoupling + scaling (M8), both
  referenced from the milestones.

- **2026-10-16 (M2.5 gate met — the main menu displays *and* is navigable, on a
  live window)** — the title takes an **A** press (**Start does not**) and opens
  the four-entry startup menu — NEW GAME (selection bar), MYSTERY GIFT, NINTENDO
  WFC SETTINGS, MIC TEST — on engine A with legible text over the trainer art and
  engine B blank, no CONTINUE under `--no-save`, ~2.4 s after the press (a "press
  start" beat, then a fade). Captured twice headless (`banks/shots-start/`,
  `banks/shots-menu2/`) and then decisively **live**: `banks/probe_live.py` drives
  the `--interactive` window closed-loop through the **frontend's own** key
  injection (`frontend_input` `action=key` — real SDL KEYDOWN/KEYUP into the same
  handler the keyboard uses) while the runner pumps the debug server
  (`main.cpp:2273`), and `banks/shots-live/` shows the bar moving
  NEW GAME → MYSTERY GIFT → NINTENDO WFC SETTINGS → MYSTERY GIFT under
  Down/Down/Up, then A opening MYSTERY GIFT's own four-entry menu (RECEIVE GIFT /
  CHECK THE CARD ALBUM / INFO / QUIT) with "Welcome to Mystery Gift!" on the
  **bottom** screen, whose cursor then moves too. Both halves of the gate hold,
  through recompiled guest code + the HAL. Probe caution: key injection is not
  self-confirming — two of four title presses were ignored with no identified
  difference in the driving method (one ignored press is 0.25 s in emulated time
  from an accepted one), so claims must be read from the pixels. Details:
  `re/notes/recompilation.md` §"M2.5 gate".

- **2026-10-16 (the uncommitted bank regeneration could not boot Black — root
  cause found: the ARM7 alias banks)** — `banks/black` regenerated at 15:30 from
  the in-flight tree (the two ARM7 `[[code_copy]]` aliases plus the 1387 ARM7
  seeds that §"Coverage" in `re/notes/recompilation.md` asks for;
  `tools/bankgen/main.cpp` +250 lines, `tools/stage_title_config.py`, both
  `re/config/black/seeds/*_unresolved.toml` folded in) left the ARM9 parked in the
  crt0 wait-stub (`pc9=0x01FF8010`, `irq9=0 irq7=0`, `vblank9=0` at t=40 s) while
  the **same runner** boots under `--force-tier3` and the **pre-regeneration**
  bank archive boots normally — so the regression was in the banks, not the runner
  and not the `NDS_GPU2D_THREADED=0` wedge workaround. **Bisected the same day**,
  one regeneration + rebuild per step, all three builds boot-tested: aliases off
  (every seed file left on) **boots** — `vblank9` 41 → 2376, splash
  `b45f04f74705`; aliases on with the ARM7 seed files off **hangs** — so it is the
  ARM7 **alias bank's own code**, not the seeds, which cannot even be applied
  without the aliases (all 1387 drop as "outside image", leaving the original
  49-function ARM7 bank). The ARM9 half of the change is sound: 5265 → 7175
  functions from its 937 seeds, and it boots. `re/config/black.toml` now keeps the
  whole change except the two `[[alias]]` blocks, commented out with the reasoning
  at the site; `banks/black` was regenerated from it and `nds_runner` rebuilt, so
  the binary on disk boots. The mechanism is still open — see
  `re/notes/recompilation.md` §"The ARM7 alias banks".

- **2026-10-16 (the game is on screen — live window)** — the runner's
  `--interactive` frontend had never been exercised; it opens a 512×768
  top-over-bottom SDL window, maps the keyboard to the DS buttons, and is now
  opened by the checked-in `tools/play.ps1` / `tools/play.sh`. Verified with a
  live window: 600 presented frames in 12.166 s (49.3 fps) and 280 frames
  (44.8 fps, **zero** audio underruns), with the **boot splash on the top
  screen and the bottom screen blank** — the frontend's own exit hash reports
  different top/bottom digests (`f462d3ff…` / `06be2b0d…`), matching the
  headless capture. Two corrections this forced: (a) the port's **pacing is the
  audio queue**, not a timer or vsync, so no audio device means an unpaced
  window; (b) the recorded "~26 % of real-time" figure divided *ARM9 cycles* by
  wall time, and ARM9 cycles stall while the CPU is halted, so it is not a
  throughput ratio — presented-frame rate is, and it is **~45–50 fps** here.
  The window does **not** touch the ≈83.7 s wedge, which is the next
  workstream.

- **2026-10-16 (FIRST PIXELS — the M2.3 gate is met)** — the boot splash
  renders. Engine A enables **BG1 at `vblank9=240` (~4 s of emulated time)** and
  the captured frame is the Pokémon Company + Nintendo logo, with the bottom
  screen correctly blank. Captured, hashed and dumped with the new
  `tools/runwatch.py`, which drives the runner's debug server (`--serve`) and
  logs a progress curve instead of a single endpoint. Three things made this
  visible where the previous session saw nothing: the Debug build was `-O0`
  (the "26 % of real-time" margin quoted there measures ARM9 cycles, not frames
  — see the live-window entry above; what survives is that the ~52 s fixed
  startup is really ~3.3 s optimized); a sampler that watches the running game
  rather than one fixed budget; and a **near miss** — the old negative stopped
  at 223 vblanks, 17 short of the layer turning on. Next: title → Start → main
  menu (M2.5) and the ARM7 coverage work. Logs, timeline JSONL and frames live
  in git-ignored `banks/`; method and hashes are in
  `re/notes/recompilation.md`.
- **2026-10-16 (the intro cinematic renders)** — continuing past the splash
  renders the **entire Pokémon Black intro**: the Game Freak logo, the
  castle/throne artwork, Zekrom in flight, the human characters and the closing
  "You and Your Pokémon!" card — text, sprites, fades and full-colour 2D
  artwork, all from the recompiled game (146 distinct frames captured). The run
  then **stops progressing at t≈83.7 s emulated** (`vblank9`≈4986, i.e. right at
  the end of the intro), and this is **not** a sampling artifact: a
  `run_rounds` call that costs 0.2 s earlier in the run does not return within an
  hour. `vblank9` is a guest-controlled IRQ counter (it only ticks while the
  guest leaves DISPSTAT bit 3 set — see `re/notes/recompilation.md`), so the
  harness now drives on scheduler rounds and reads VCOUNT/DISPSTAT as ground
  truth, with a driver timeout so a wedge is reported instead of burning an
  hour. **Root-caused 2026-10-16:** `banks/probe_step.py` pinned it to a single
  scheduler round (#42793, t=83.692661 s; ARM9 `pc=0x1FF8098` in ITCM, ARM7
  asleep in freebios), and sampling Win32 CPU time from *outside* the process
  while the call was in flight shows it burning **193 % of one core** — a host
  spin, not a guest deadlock and not an unbanked jump (no `[dispatch-miss]`, no
  stub flood). **Resolved the same day: it is the vendored `gpu2d` threaded
  scanline path.** With the 2D raster back on the emu thread
  (`NDS_GPU2D_THREADED=0`) the same drive runs **clean to t=95.03 s**; with the
  default (2 workers) it wedges at the same round. That is a configuration-level
  workaround needing no fork — the runner documents the inline path as the
  identical latch/execute sequence, byte-locked to the threaded one against the
  presented-frame digest ring, so it costs wall time, not pixels. Bring-up runs
  now set it, and the runner bug goes upstream with the round-level reproduction.
  It is **not** the coverage problem, which is separate and now specified (next
  entry).
- **2026-10-16 (coverage gaps specified by the submodule's own ingest)** —
  `third_party/ndsrecomp/tools/ingest_coverage_manifests.py`, run over our seven
  runner-written manifests, resolves both long-open gaps exactly: `arm7.bin`
  executes from **two undeclared bases** (`0x02FCE5A8`, `0x037F79BC`) while the
  only ARM7 bank sits at `0x02380000` — so *all* ARM7 execution was interpreted —
  and the ARM9 holes inside its declared range enumerate to 937 observed entry
  points, hottest at the block `0x020834E8..0x02083590` that no generated
  function covers. Both fixes are config-level and need no dumped bytes:
  recompile the same `arm7.bin` at each resident base (base =
  `guest_span_start - module_span_start`), and seed the ARM9 holes. Counts and
  the per-base seed files are in `re/notes/recompilation.md`; the seeds go into
  checked-in per-title configs under `re/config/`, with the generated banks
  staying git-ignored.

- **2026-10-16 (late platforms recorded; bring-up presets added)** — `Plan.md`
  gains **M10: Android, then web (WebAssembly)** as *recorded, unscheduled*
  host targets — far future, after M8/M9. The part that binds now: no run-time
  code generation anywhere (a web build cannot host it, a second reason
  `docs/adr/0003-tier3-interpreter-policy.md` is load-bearing), no hosting or
  fetching of ROM content (a web build takes the user's own local file), the HAL
  seam stays the only path to devices, and GPL-3.0 source ships with any WASM
  binary. `AGENTS.md` §1 states the same. Also added
  `windows-relwithdebinfo` / `linux-relwithdebinfo` presets, because every
  bring-up measurement so far came from an `-O0` Debug build.

- **2026-10-16 (bring-up review: the first-screen plan is fixed)** — a design
  review settled the M2.3→M2.5 push and wrote it into the M2 preamble: target is
  Black-only logos → title → Start → menu; the gate is **pixels** (proven from
  the runner's debug server, `framebuffer A|B`), not fps; Tier 3 stays a floor
  but must cover **<0.1 %** of executed instructions (new
  `docs/adr/0003-tier3-interpreter-policy.md`); overlays get banked ahead of
  time; ARM7's run-time copies get declared; per-title configs are checked in
  under `re/config/`; and the vendored submodule stays pristine. Two findings
  forced the reordering: **every earlier throughput number came from a Debug
  runner** (`build/windows-debug`, 49 MB) and is therefore void, and the runner
  has a **JSON debug server** (`--serve`, `TCP.md`) that can return both
  framebuffers — so automated frame evidence needs no fork. Corrected the stale
  "gitlink must record the tag" hazard in `re/notes/recompilation.md`: HEAD
  already carries `aa40b575` (`v0.6.10`) and both `.gitmodules` entries. Not yet
  done: the re-measurement, the declarations, and the overlay bank pipeline.

- **2026-10-16 (M2.3 attempt: Black boots, logos do not render yet)** — the
  runner is in the build and Black boots: the banks register against the ROM
  SHA-1, both CPUs execute, and the ARM9 passes `NitroMain` into NitroSDK init
  (so the M2.1 gate holds). The M2.3 gate does **not**: after 250 M ARM9 cycles
  (3.7 s of DS time) no BG/OBJ layer is ever enabled, so nothing is displayed.
  Cost is ARM7-dominated (71 % of scheduler time): 1.27 M of the ARM7's 1.37 M
  Tier-3 entries are unbanked **WRAM/shared-WRAM code**, not the BIOS. **These
  figures were first recorded on a Debug runner and partly wrong; see the
  re-measurement in `re/notes/recompilation.md` — true marginal throughput is
  ~6.3 % of real-time, not 0.75 %.** Boot command, measurements and unknowns are
  in `re/notes/recompilation.md`. No push (per the no-push-until-logos rule —
  since replaced by "push any license-safe working state").

- **2026-10-16 (M2.1 substrate: runner in the build + Black title banks)** —
  Black's ARM9/ARM7 banks now generate and the adopted runner is part of the
  build (`PKMNG5PC_BUILD_RUNNER`); overlays are left to the runner's live path.
  Two findings re-scoped the plan: the runner HLEs **no** NitroSDK/FS/OS surface
  (it refuses load-bearing HLE and emulates the cart instead), so **M2.2 is LLE,
  not HLE** and `rom/` is an **offline** library; and there is **no retail-title
  precedent** in `ndsrecomp` — we are the first. Details and evidence in
  `re/notes/recompilation.md`.

- **2026-10-16 (review + course corrections)** — reviewed the previous session's
  work and ratified it with the maintainer: **strategy stays static
  recompilation**, and **GPL-3.0-or-later is adopted deliberately** (we reuse
  the melonDS 3D GE + Wi-Fi; melonDS is GPL-3.0-or-later and no permissive DS 3D
  renderer exists). Decisions recorded in `docs/adr/0001-static-recompilation.md`
  and `docs/adr/0002-gpl-adoption.md`; language pillared in `GLOSSARY.md`.
  The previous `engine/` library is the **ROM & Nitro format layer**, not the
  game engine: renamed `engine/` → `rom/`. Removed the 332 MB `extracted/` tree
  (regenerable from the ROM) and the `temp.md` scratch. Still open: wiring
  `ndsrecomp` into the top-level build (today only `rom/`, `tools/`, `tests/`
  build). M2.1 next: install SDL3 (fetch from github.com —
  `raw.githubusercontent` is blocked), generate the Black ARM9/overlay/ARM7
  title banks (`NDS_TITLE_BANK_DIR`; needs a recompiler config for arm9 + 237
  overlays), then attempt a headless boot.

- **2026-10-14 (ndsrecomp submodule + builds)** — `ndsrecomp` @ `v0.6.10` is a
  **pinned git submodule** at `third_party/ndsrecomp` (`aa40b575`). Fresh-clone
  bootstrap: `tools/bootstrap_ndsrecomp.ps1` / `.sh` — inits the submodule +
  nested `arm-recomp-core`/`freebios`, fixes the broken relative
  `arm-recomp-core` URL (also recorded as a parent `.gitmodules` override),
  stages `toml.hpp` from `marzer/tomlplusplus` v3.4.0 (`raw.githubusercontent`
  is blocked), generates the FreeBIOS + placeholder retail BIOS banks, and
  builds. **Recompiler builds** (`nds_recompile`); `ctest` 5/5. **Runner
  builds** (`nds_runner`, headless, `NDS_SDL_BACKEND=NONE`,
  `NDS_BOOTSTRAP_FIRMWARE=ON`); `ctest` 32/33 (`gpu2d_window_test` =
  `STATUS_STACK_OVERFLOW`, a Windows stack-size quirk in the unwind test). The
  kept melonDS **3D GE (GPU3D.cpp, GPU3D_Soft.cpp, Savestate.cpp)** and **Wi-Fi
  (Wifi.cpp, WifiAP.cpp, net/, libslirp)** + bridges (`gpu3d.cpp`,
  `wifi_net.cpp`) all compile — validating the GPL adoption. Retail-BIOS banks
  are placeholders built from the FreeBIOS images (runtime uses FreeBIOS by
  default). Blocked on boot: retail `firmware.bin` banks, Black title banks,
  SDL3.

- **2026-10-14 (license migration)** — switched this project to
  **GPL-3.0-or-later** (replaces the earlier MIT decision). Adopt `ndsrecomp` @
  `v0.6.10` **as-is**: keep its melonDS-derived 3D GE (software + compute) and
  Wi-Fi. No melonDS/GPL stripping and no 3D/Wi-Fi stubs. `LICENSE` rewritten,
  `NOTICE`/attribution updated. GPL is confined behind `gpu3d.h` / `wifi.h`;
  the runner core stays clean for later re-forking if ever desired.

- **2026-10-14 (M2.0, D7/D8 resolved)** — recompiler + runner chosen: adopt
  `ndsrecomp` @ **`v0.6.10`** (**superseded by the GPL migration above**; `main`
  is PolyForm Noncommercial) (**option 1: faithful floor**). Method family = static recompilation (**Sonic Unleashed Recompiled /
  Zelda64Recomp**), not decomp-port (**Ship of Harkinian** — no Gen 5 decomp).
  Graphics + sound come from the runner (3D GE + ARM7 sound), not stubbed.
  D8: BIOS HLE via `freebios` (BSD-2). See `re/notes/recompilation.md`.

- **2026-10-14 (M2.0)** — boot analyzed; recompiler tooling decided.
  - `re/notes/arm9-init.md`: located `_start` @ `0x02004800` (NitroSDK crt0:
    IME off, CP15 init, `0x01FF8000` stub copies, per-mode stacks, VBlank wait)
    and THUMB `NitroMain` @ `0x02005124` (entered via `0x02005125`, return-to-
    BIOS `0xFFFF0000`). Evidence IDs `A9-*`.
  - `re/notes/recompilation.md` (D7): initially proposed a minimal in-house
    recompiler; **superseded** — adopt `ndsrecomp` @ `v0.6.10`. `main` is
    PolyForm Noncommercial (unusable); `Myth7x/ndsrecomp` is unlicensed;
    `freebios` (BSD-2) is usable. License posture later migrated to
    GPL-3.0-or-later (see newer entry). D8 = BIOS HLE.
  - Seeded `re/symbols/` (format README + `black-arm9.json`).
  - Disassembly harness: wrap decoded `arm9.bin` as an ARM ELF via
    `llvm-objcopy`, disassemble with `llvm-objdump` (ARM + THUMB).

- **2026-10-14 (M2 plan)** — D1 resolved as **static recompilation (B)**.
  - §2 rewritten: Gen 5 decomps not usable; DS static recompilation proven
    viable. Reuse an existing recompiler where license-permitted; no emulator
    core is shipped.
  - M2 split into **M2.0–M2.5** (RE setup → execution substrate → FS/OS HLE →
    video HLE/renderer → input/timing/audio → main menu). First visual target is
    M2.3 (2D logos); Windows and Linux in lockstep.
  - New open decisions **D7** (recompiler source) and **D8** (BIOS handling).

- **2026-10-14 (later)** — M1 extraction pipeline landed.
  - `engine/`: added `sha256` (known-answer tested) and `extract` module
    (`to_directory`, `manifest_text`); `narc::file_data` now returns empty
    members instead of an error.
  - `tools/romdump`: `--extract <dir>` writes `nitrofs/<path>`, decoded
    `overlays/ovNNN.bin`, `arm9.bin`, expanded `narc/...` members, plus a
    `manifest.sha256` (`sha256sum` format). `--narc-limit N` caps expansion.
  - Verified on Black: 54539 files, 0 skipped; full manifest is byte-identical
    across two runs and independently re-hashed with an external SHA-256 tool.
  - Tests: `sha256_known_vectors`, `extract_manifest_deterministic` (extracts a
    subset twice and re-verifies every hash/size). All green.
  - M1 remaining: NARC member format identification and the 2D/3D/audio parsers.

- **2026-10-14** — M0 complete; M1 parsing/codecs landed.
  - Foundation: CMake presets (win/linux × debug/release), warnings target,
    clang-format/editorconfig, GPL-3.0-or-later `LICENSE` + `NOTICE`, REA note
    templates.
  - Library (`engine/`, target `pkmn::core`): `compression` (LZ10, LZ11,
    Huffman4/8, RLE, **BLZ**), `nds` (header, overlays, FAT, FNT walker),
    `narc` (archive parsing).
  - Tools (`pkmn_romdump`): header dump, ARM9 BLZ size, `--list`, `--narcs`,
    `--overlays`.
  - Tests (`pkmn_tests`, run via `ctest`): codec round-trips + bad-input, plus a
    ROM-driven inventory test (header, ARM9 BLZ, FNT/FAT, NARC, all overlays).
  - Verified on all four ROMs (SHA-256 in `re/notes/header.md`).

