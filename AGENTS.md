# AGENTS.md

Guidance for AI agents and human contributors working in **pkmng5pc**.

## 1. What this project is

A clean native PC reimplementation ("source port") of the Generation V Pokémon
games for **Windows and Linux**:

| Game | Region | ROM size |
| --- | --- | --- |
| Pokémon Black | USA/EU (NDSi Enhanced) | 256 MiB |
| Pokémon White | USA/EU (NDSi Enhanced) | 256 MiB |
| Pokémon Black 2 | USA/EU (NDSi Enhanced) | 512 MiB |
| Pokémon White 2 | USA/EU (NDSi Enhanced) | 512 MiB |

The ROM is whatever dump the user drops into `roms/`; refer to it by path
(e.g. `roms/<your-rom>.nds`), never by a dump's release filename.

Goals: both generations playable from **one launcher/menu**, plus later
enhancements (any-aspect dual-screen layout, post-FX including bloom and shadow
maps, decoupled presentation at a locked 60 Hz logic rate, New Game+, and a
forced-rule Nuzlocke mode — see `Plan.md`). The **modding suite and modding
agent skills are a separate project** and are out of scope for this repo.

**Target platform:** Nintendo DS / NDSi, ARM9 (ARMv5TE) + ARM7 (ARMv4T).

**Host targets:** Windows and Linux today. **Android and web (WebAssembly) are
recorded late goals** (`Plan.md` M10) — not scheduled, and not a reason to add
abstractions now, but constraints not to foreclose: game logic stays away from
OS/GPU APIs (§5), nothing may depend on run-time code generation (a web build
cannot host it — `docs/adr/0003-tier3-interpreter-policy.md`), and a web build
must take the user's own ROM as a local file, never host or fetch it (§2.5).

## 2. Legal rules — read before touching anything

These are hard constraints. Violating them can kill the project.

1. **No copyrighted game content is ever committed.** No ROMs, no extracted
   graphics/audio/text/scripts/models, no decrypted assets, no save files with
   proprietary content. `roms/` and any `extracted/` output are **git-ignored**.
2. **The user must supply their own legally obtained ROMs** at build time or
   run time. The shipped program contains no Nintendo/Game Freak assets.
   Never record a specific dump's filename/release name in the repo (docs,
   notes, comments, scripts, tests) — say "the ROM path" or
   `roms/<your-rom>.nds` instead.
3. **Asset extraction is performed locally by the user's own tool**, from the
   user's own ROM. Do not redistribute extracted data.
4. **Reverse-engineered knowledge (formats, addresses, decompiled logic) is
   fine to document and share**; the *content* it describes is not.
5. Do not add network features that download or share ROMs, assets, or saves of
   others.
6. Keep `.gitignore` exhaustive: `roms/`, `*.nds`, `*.sav`, `*.dsv`,
   `extracted/`, `build*/`, `*.idb`, `*.i64`, `*.gpr`, `*.rep`.
7. **License: this project is GPL-3.0-or-later.** It adopts `ndsrecomp`
   (incl. its GPL-3.0 melonDS 3D GE and Wi-Fi subtrees) as-is. Do not add
   GPL-incompatible or proprietary code. Keep GPL-derived code behind the
   established bridges (`runner/src/gpu3d.h`, `runner/src/wifi.h`) and record
   third-party licenses in `NOTICE`.
8. **Asset provenance.** AI may generate development-only placeholders, but
   **nothing AI-generated ships**. Shipping visual and audio assets are
   hand-made by the user, and the build must not package AI-generated art,
   audio, or models.

If a task would require committing or redistributing game content, **stop and
ask** — offer the ROM-at-runtime alternative instead.

## 3. Repository layout

```
pkmng5pc/
├─ AGENTS.md              # this file
├─ Plan.md                # roadmap, milestones, risks
├─ roms/                  # user-provided ROMs (git-ignored, never commit)
├─ tools/                 # local RE + extraction tooling (our code)
│  ├─ romdump/            # NitroFS/NARC/overlay extractor
│  ├─ formats/            # NCGR/NCLR/NSCR/NSBMD/SDAT parsers
│  └─ scripts/            # Ghidra/REA import helpers
├─ re/                    # RE artifacts: notes, symbol maps, REA bundles
│  ├─ notes/              # per-subsystem findings (Markdown)
│  ├─ symbols/            # function/globals names (JSON/IDC/Ghidra)
│  └─ evidence/           # exported REA evidence bundles
├─ rom/                   # ROM & Nitro format layer (NitroFS/NARC/codecs)
├─ platform/              # win32/linux now; android/web later (M10) — window, input, fs, audio device
├─ render/                # modern renderer + DS-GE emulation layer
├─ game/                  # recompiled/reimplemented game logic
├─ launcher/              # main menu, ROM selection, version switching
├─ tests/                 # unit + golden-data tests
├─ patches/               # tracked local patches to pinned third-party sources
└─ third_party/           # pinned submodules (ndsrecomp, freebios)
```

`extracted/` (built assets) lives outside VCS and is produced by `tools/`.

## 4. Technology stack

- **Language:** C++20 (matches the RE/decomp ecosystem; Ghidra and pret decomps
  are C/C++). Rust is an acceptable alternative for *new* isolated components,
  not for recompiled game code.
- **Build:** CMake ≥ 3.24 + Ninja/MSVC/Clang. Presets in `CMakePresets.json`.
- **Windowing/input/audio:** SDL3 (SDL2 fallback).
- **GPU:** a single abstraction (`render/backend.h`) with backends: **Vulkan**
  (Linux+Win), **D3D12** (Win), **OpenGL 4.5** (fallback/debug). Use a thin
  layer (e.g. bgfx or sokol or hand-rolled RHI) — do **not** scatter API calls.
- **Math/asset:** glm; nlohmann/json for config; zstd for local asset caches.
- **Save I/O:** byte-exact `.sav` reader/writer (see §7).

Do not add a dependency before checking whether the recompiler or runner
already ships an equivalent. This project is **GPL-3.0-or-later** (§2.7); never
vendor GPL-incompatible or proprietary libraries.

## 5. Architecture (target)

```
launcher (menu, ROM pick, settings, version switch)
        │  selects ROM + config
        ▼
game  ── recompiled Gen5 logic  ──┐
        (ARM9/ARM7 → native, or    │ calls
         decompiled game code)     ▼
                         hardware abstraction layer (HAL)
        ┌──────────────┬──────────┬───────────┬──────────┐
        ▼              ▼          ▼           ▼          ▼
    DS 2D engine   DS 3D (GE)  ARM7 sound   input    NitroFS/ROM
    (BG/OBJ)       emulation   (SDAT/SSEQ)  (btn/touch) (NARC/overlay)
        │              │          │
        └────────► render/ compositor ◄────────┘
                         │
                    modern backend (Vulkan/D3D12/GL)
                         │  post-FX, shadows, widescreen
                         ▼
                     window / swapchain
```

Key principle: **game logic must not call the GPU or OS directly.** Everything
goes through the HAL. That is what makes widescreen/shadow/60-fps enhancements
possible later without touching game logic.

The cart/NitroFS box is the runner's **emulated cartridge**, not our `rom/`
library: `ndsrecomp` recompiles the game's own NitroSDK code and refuses
load-bearing HLE (`Plan.md` M2.2). `rom/` is therefore an **offline** layer —
bank generation, save I/O, launcher ROM detection — and never runs inside the
game loop.

The DS has two screens (256×192 each). Define canonical internal resolution and
map both screens for 16:9 output (§8 in `Plan.md`).

## 6. Reverse-engineering workflow (REA)

Claims about the ROM must be evidence-backed. Use the REA skill/tooling.

1. **Bind the target** with `open_binary` on the exact ROM path; record the
   SHA-256 and ROM header (title, game code, ARM9/ARM7 entry points, overlay
   table, FNT/FAT offsets).
2. **Inventory in layers:** ROM header → NitroFS tree → NARC archives → member
   files (NSBMD, NCGR, SDAT, text, scripts).
3. **Attribute findings to Evidence IDs.** Separate **observations** (what a
   tool returned) from **inferences** (what you conclude) from **unknowns**.
4. **Persist notes** in `re/notes/<subsystem>.md` with: ROM SHA-256, file
   offsets, Evidence IDs, tool versions, confidence.
5. **Never** infer behavior from a filename or a string alone.
6. Export large analyses to `re/evidence/` with `export_evidence_bundle`.

Per-subsystem note files to maintain: `header`, `nitrofs`, `overlays`,
`arm9-init`, `map-system`, `script-vm`, `text-system`, `pokemon-data`,
`battle`, `save`, `3d-ge`, `sdat-audio`.

Cross-ROM comparison (Black vs White, BW vs B2W2) is a **first-class** task:
shared format, differing data. Diff by SHA-256 + offset, not by eyeballing.

## 7. Save compatibility

- Gen 5 saves are 0x24000 bytes; the game uses a block/rotation + checksum
  scheme. Read and write **byte-exactly**; verify with in-game load.
- Support at least: import existing `.sav`/`.dsv`, export back, and NS+ carry
  (see `Plan.md`).
- Any save-touching change requires a golden-file test: known-good save in →
  expected save out (byte compare), plus a round-trip test.

## 8. Coding conventions

- Follow the standard in neighboring files; match existing style over preference.
- **No comments unless asked.** Prefer self-documenting names.
- Game-logic files mirror the decomp structure (`src/`) verbatim when recompiling
  so upstream decompile updates can be merged. **Do not refactor decompiled
  code**; isolate its assumptions in the HAL.
- Formatting: `.clang-format` (WebKit-based), `.editorconfig`. Naming: `snake_case`
  functions/vars, `PascalCase` types, `SCREAMING_SNAKE` constants and macros.
- Fixed-width integers (`u8/u16/u32/s32/u64`) for anything ROM-derived.
- All ROM reads are **bounds-checked**; never trust an offset or size.
- One source of truth per subsystem; no duplicated tables.

## 9. Build & test

```sh
cmake --preset linux-debug    # or windows-debug
cmake --build --preset linux-debug
ctest --preset linux-debug
```

(Mirror in `windows-*` presets. If a preset/script is missing, ask before
inventing one and add it here once confirmed.)

**ndsrecomp submodule.** The static-recompilation toolchain `ndsrecomp`
(recompiler + runner) is a pinned git submodule at `third_party/ndsrecomp`
(tag `v0.6.10`). Bootstrap and build it with `tools/bootstrap_ndsrecomp.ps1`
(Windows) or `tools/bootstrap_ndsrecomp.sh` (Linux/macOS); add `-BuildRunner` /
`--build-runner` to also build the runner. The script inits the submodule, fixes
the broken `arm-recomp-core` nested URL, stages the toml++ header, generates the
FreeBIOS + placeholder retail BIOS banks, and builds. `-Compiler` / `--compiler`
selects the toolchain (C name given, C++ sibling derived); generated banks
under `third_party/ndsrecomp/generated/` are git-ignored (regenerable).

Configure applies the tracked patches in `patches/ndsrecomp/` to the submodule
(idempotently), so the superproject reports it as *modified content* by design:
two today — `0001-native-present-hook` (the guarded present hook behind
`tools/play -Native`, `docs/adr/0006-ndsrecomp-native-present-hook.md`) and
`0002-libslirp-msvc-clang` (POSIX fd/socket calls in the vendored libslirp,
which only compile by accident on toolchains whose headers declare them). Revert
with `git -C third_party/ndsrecomp checkout .` and re-configure. Both patched
files still build standalone (the hook compiles out without our build's macro,
and 0002 is a portability fix that is correct on POSIX too), so
`bootstrap_ndsrecomp.*` needs no change.

- **Unit tests:** format parsers, compression codecs, save checksums.
- **Golden data:** small, synthetic fixtures only — **never** commit real ROM
  data. Generate fixtures from user ROMs locally at test time.
- **Visual/regression:** frame-hash captures against a locally rendered
  reference for the renderer.
- Run the linter/formatter before finishing any task.

## 10. Agent do / don't

**Do**
- Ask which ROM/app if a target isn't identified.
- State observations vs. inferences vs. unknowns for every RE finding.
- Keep ROMs/assets out of commits; verify `.gitignore` before staging.
- Prefer reusing existing tools (Ghidra, DSPRE, Tinke, Nitro tooling) over
  writing a new parser for an established format.
- Record Evidence IDs and tool versions in notes.
- Ship or commit hand-made assets only; AI output is development-only and never
  packaged.

**Don't**
- Commit ROMs, saves, or extracted assets.
- Claim runtime behavior from static analysis.
- Freeze an architectural decision that `Plan.md` marks as *open*; propose and
  ask instead.
- Expand scope to the enhancement features before the core port milestone.
- Commit or package AI-generated visual/audio assets.

## 11. Glossary

Format acronyms and shorthand used in this file. Project vocabulary
(recompiler, runner, ROM layer, bank, oracle, …) lives in `GLOSSARY.md`.

- **NitroFS** — NDS ROM filesystem (FNT + FAT).
- **NARC** — Nitro ARChive; container inside NitroFS.
- **Overlay** — dynamically loaded ARM9 code (`ovNNN`, often compressed).
- **GE** — DS 3D Geometry Engine (fixed-function).
- **BG / OBJ** — DS 2D tile layers (backgrounds) and sprites (objects).
- **NSBMD/NSBTX/NSBMA/NSBTA** — Nitro 3D model / texture / material+model anim /
  texture anim.
- **NCGR/NCLR/NSCR/NCER/NANR** — Nitro tile graphics / palette / tilemap /
  cell / animation formats.
- **SDAT/SSEQ/SBNK/SWAR/SWAV/STRM** — Nitro sound archive and its members.
- **SCRCMD / script VM** — Gen 5 overworld script bytecode interpreter.
- **NG+ / New Game Continue** — New Game Plus, and the cross-title carry that
  starts a harder B2W2 run from a completed Black save (project features,
  `Plan.md`).
- **HAL** — hardware abstraction layer (see §5).
