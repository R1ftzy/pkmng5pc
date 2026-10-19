# pkmng5pc

A clean native PC (Windows/Linux) reimplementation of Pokémon Black/White and Black 2/White 2, driven by user-supplied ROMs. See `AGENTS.md` for contributor guidance and `Plan.md` for the roadmap.

This repository contains **no game content**. You must own and supply the ROMs.

## Quick start (Windows)

```sh
# 1. Bootstrap the recompiler submodule (once; inits ndsrecomp, builds nds_recompile)
pwsh tools/bootstrap_ndsrecomp.ps1

# 2. Build this project's host tooling (CMake >= 3.24 + clang++ + ninja on PATH)
cmake --preset windows-debug
cmake --build --preset windows-debug

# 3. Put your own Black ROM in roms/ (any filename; see "ROMs")

# 4. Generate the title banks from your ROM (writes the git-ignored banks/black/)
build/windows-debug/tools/pkmn_bankgen.exe "roms/<your-rom>.nds" banks/black `
    --title-config re/config/black.toml `
    --recompiler third_party/ndsrecomp/recompiler/build/nds_recompile.exe

# 5. Configure and build the runner (first configure clones and builds SDL3)
cmake --preset windows-relwithdebinfo -DPKMNG5PC_BUILD_RUNNER=ON
cmake --build --preset windows-relwithdebinfo

# 6. Play (loads roms/<your-rom>.sav; -NoSave for a blank cart)
pwsh tools/play.ps1
```

**Controls** (the live window's keyboard map):

| Keys | Button |
| --- | --- |
| `arrows` | D-pad |
| `Z` / `X` | A / B |
| `A` / `S` | Y / X |
| `Q` / `W` | L / R |
| `Enter` | Start |
| `Backspace` | Select |
| `Esc` | quit |

The bottom-screen window is the touchscreen.

On Linux / macOS the same steps use `sh tools/bootstrap_ndsrecomp.sh`, the `linux-*` presets, the tools without the `.exe` suffix, and `sh tools/play.sh`. The full detail behind each step is in [Build](#build), [Runner](#runner-the-bootable-build) and [Run](#5-run).

## Layout

- `rom/` — ROM & Nitro format layer (headers, NitroFS/FNT/FAT, NARC, codecs)
- `tools/` — local tooling: `romdump` (extraction), `bankgen` (title banks)
- `re/` — reverse-engineering notes and evidence
- `tests/` — unit and golden-data tests
- `cmake/` — build integration: the `ndsrecomp` runner + SDL3 superbuild
- `banks/` — generated title banks (git-ignored; produced from your own ROM)

## Requirements

- **CMake >= 3.24** and a generator: **Ninja** (recommended) or Visual Studio.
- A **C++20** compiler: `clang++` on Windows (LLVM-MinGW works and is what the presets pin) or `gcc`/`clang` on Linux.
- **Git** — `third_party/ndsrecomp` is a pinned submodule.
- Network access on the first *runner* configure: CMake clones SDL3 from GitHub. (The clone uses `git`, so it still works where `raw.githubusercontent.com` is blocked.)

SDL3 is **not** a manual prerequisite — the runner configure fetches and builds it automatically (see [Runner](#runner-the-bootable-build)). Nothing outside `rom/`, `tools/` and `tests/` needs it.

Windows: the presets pin `CMAKE_CXX_COMPILER=clang++`. Put `clang++`, `cmake`, `ninja` and `ctest` on `PATH`.

## Build

Every ROM-independent target (`rom/`, `tools/`, `tests/`) builds with no ROM and needs no submodule checkout:

```sh
# Configure (pick the preset for your OS)
cmake --preset windows-debug          # or: linux-debug

# Build
cmake --build --preset windows-debug

# Run the test suite
ctest --preset windows-debug
```

Presets (`CMakePresets.json`): `windows-debug`, `windows-release`, `linux-debug`, `linux-release`. Output goes to `build/<preset>/`; the test binary is `build/<preset>/tests/pkmn_tests[.exe]`, the tools are under `build/<preset>/tools/`.

Once the runner is enabled, `ctest` also runs `ndsrecomp`'s own tests. Two of those are environment-sensitive on Windows and can fail without affecting the runner — see [Troubleshooting](#troubleshooting).

## Runner (the bootable build)

Running the game needs **title banks** recompiled from your own ROM plus the `ndsrecomp` runner. This is **off by default** (`PKMNG5PC_BUILD_RUNNER=OFF`), so CI and the plain build never touch it.

Banks must exist **before configuring**: the runner globs them at configure time and fails closed on an empty group. The order below matters.

### 1. Bootstrap the recompiler

`third_party/ndsrecomp` is a submodule, and the recompiler is a standalone project. Its bootstrap script inits the submodule (including the nested `arm-recomp-core`, whose pinned URL is broken and gets repointed at GitHub), stages the toml++ header, builds `nds_recompile`, and generates the BIOS banks:

```sh
# Windows
pwsh tools/bootstrap_ndsrecomp.ps1

# Linux / macOS
sh tools/bootstrap_ndsrecomp.sh
```

Add `-BuildRunner` / `--build-runner` to also build a standalone runner under `third_party/ndsrecomp/runner/build/`. **Not needed here** — step 4 builds the runner as part of this project, which is the only way to have SDL3 supplied via CMake.

### 2. Build this project's tools

```sh
cmake --preset windows-debug
cmake --build --preset windows-debug
```

This produces `pkmn_bankgen`, which step 3 needs.

### 3. Generate the banks

```sh
build/windows-debug/tools/pkmn_bankgen.exe \
    "roms/<your-rom>.nds" banks/black \
    --title-config re/config/black.toml \
    --recompiler third_party/ndsrecomp/recompiler/build/nds_recompile.exe
```

(Drop the `.exe` on Linux.) This writes to the git-ignored `banks/black/`:

| Output | Meaning |
| --- | --- |
| `irbo_arm9_main_00..07.c`, `irbo_arm9_main_dispatch.c` | ARM9 bank: 8422 functions (5265 from the finder alone, 8422 once the 1509 seeded entry points are declared), 8 shards |
| `irbo_arm7_main_00..07.c`, `irbo_arm7_main_dispatch.c` | ARM7 bank: 3567 functions (49 discovered, then the two relocated copies and the 1387 shared seeds), 8 shards, recompiled with `--validate-live-bytes` |
| `bin/*.bin`, `config/*.toml` | decoded images and recompiler configs |

`pkmn_bankgen` derives every input from the ROM (load address, size and entry point come from the header and overlay table), so nothing ROM-derived is hand-written or committed. Options:

- `--title-config <file>` — apply the checked-in per-title config (`re/config/<title>.toml`): its `[[alias]]` entries become recompiler `[[code_copy]]` ranges for modules the game relocates, and its `[[seed_file]]` entries replay observed Tier-3 entry points as `[[entry_point]]` seeds. All of these are addresses only — no ROM bytes.
- `--shards N` — split the bank into N body shards (default 8; **minimum 2**).
- `--overlays --overlay-seeds <dir>` — additionally bank the ARM9 overlays that have an observed seed file in `<dir>`, named `arm9_ov<id>_<base>.toml` and staged by `tools/stage_title_config.py --overlays-only` from a coverage ingest (`pkmn_bankgen --overlays` without `--overlay-seeds` is an error). One overlay = one bank; the overlay table supplies the load address and size, the FAT member is BLZ-decoded when the entry is compressed, and every overlay bank is recompiled with `--validate-live-bytes` because several overlays routinely share a load address. Without the flag, only ARM9 + ARM7 static banks are generated and overlays are left to the runner's runtime live-overlay path.
- `--emit-only` — write `bin/` + `config/` and stop before running the recompiler.

Run step 3 **once per ROM you own**; each run writes its own `banks/<title>/` and prints the directory to pass as `NDS_TITLE_BANK_DIR`. Because the runner registers one bank group per ROM SHA-1 (see step 4), a single runner binary can carry several titles at once.

`re/config/` holds one config per ROM (`black.toml`, `white.toml`, `black2.toml`, `white2.toml`). The ARM7 module image is **byte-identical in all four titles**, so it is relocated to the same two bases and its observed entry points are the same list: the two `[[alias]]` blocks and the seeds under `re/config/arm7/seeds/` are shared by every title, and the ARM7 bank comes out identical for each (3567 functions). `pkmn_bankgen` recompiles any bank that declares an alias with `--validate-live-bytes`, which is what makes a relocated range sound — see `re/notes/recompilation.md` §"The ARM7 alias hang is a missing live-byte validation". The ARM9 half of a config is title-specific; every title now carries its own ARM9 seeds (`re/config/<title>/seeds/`).

#### Another title: Black 2 / White 2

Generating banks for another title is the same step with that title's config
(and, optionally, another bank directory):

```sh
build/windows-debug/tools/pkmn_bankgen.exe \
    "roms/<your-rom>.nds" banks/black2 \
    --title-config re/config/black2.toml \
    --recompiler third_party/ndsrecomp/recompiler/build/nds_recompile.exe
```

The bank directory's name is only a label: what wires it to the runner is
`-DPKMNG5PC_BANKS_DIR=banks/black2` plus
`-DPKMNG5PC_TITLE_ROM_SHA1=<that ROM's SHA-1>` (bankgen prints the SHA-1), or
the multi-title form in step 4 when one binary should serve several titles.
`tools/play` checks `banks/black` by default, so pass
`-Banks banks/black2` (`--banks banks/black2` on Linux) when the runner was
configured with a different directory, and run with
`pwsh tools/play.ps1 -Rom "roms/<your-rom>.nds"`.
`re/config/black2.toml` is Black 2-specific (it carries no `[[patch]]` and adds
the ARM9 ITCM alias); never run a Black 2 ROM through `re/config/black.toml` —
bankgen fails loudly on the unmatched patch by design
(`docs/adr/0004-declared-bank-patches.md`).

### 4. Configure and build with the runner

```sh
cmake --preset windows-relwithdebinfo -DPKMNG5PC_BUILD_RUNNER=ON
cmake --build --preset windows-relwithdebinfo
```

Use the **`-relwithdebinfo`** preset for anything you intend to measure or profile; `windows-debug` is `-O0` and its timings mislead (the first recorded boot numbers were all Debug). Debug stays for the test suite.

`PKMNG5PC_BUILD_RUNNER=ON` (a) checks that banks exist, (b) fetches and builds **SDL3 3.4.18** via FetchContent (static, tests/examples off), then (c) `add_subdirectory`s the runner with `NDS_BOOTSTRAP_FIRMWARE=ON`, `NDS_SDL_BACKEND=SDL3`, `NDS_ENABLE_COMPUTE_RENDERER=OFF`. The first configure clones and builds SDL3, so it takes several minutes.

Two cache variables select which ROM the banks belong to:

| Variable | Default | Purpose |
| --- | --- | --- |
| `PKMNG5PC_BANKS_DIR` | `banks/black` | bank directory from step 3 |
| `PKMNG5PC_TITLE_ROM_SHA1` | Black's SHA-1 | the ROM those banks came from |

That pair wires **one** title (Black). The runner also accepts a **multi-title**
form, and that is what the current `-relwithdebinfo` binary is configured with:
`NDS_TITLE_BANK_DIRS` and `NDS_TITLE_ROM_SHA1S` are parallel semicolon lists of
extra bank directories and their ROM SHA-1s (the runner's own variables,
passed through as-is). Run step 3 once per ROM, then:

```sh
cmake --preset windows-relwithdebinfo -DPKMNG5PC_BUILD_RUNNER=ON \
    -DNDS_TITLE_BANK_DIRS="banks/white;banks/black2;banks/white2" \
    -DNDS_TITLE_ROM_SHA1S="<white>;<black2>;<white2>"      # each title's SHA-1
cmake --build --preset windows-relwithdebinfo
```

At boot the runner registers the group whose ROM SHA-1 matches the ROM being
run, so one binary serves all four titles; a group whose SHA-1 does not match
is left unregistered (`pkmn_bankgen` prints each bank's ROM SHA-1, so the cache
values come straight from the four step-3 runs).

### 5. Run

```sh
# Windows
pwsh tools/play.ps1

# Linux / macOS
sh tools/play.sh
```

`tools/play` finds the built runner, picks the Black ROM in `roms/` by default, checks that the banks exist, and opens the live SDL window with the correct boot flags. Run another title with `--rom <path>`: the runner registers that ROM's bank group by SHA-1 at boot. Either script's `--help` lists everything; the ones worth knowing:

| Option | Effect |
| --- | --- |
| `--rom <path>` | use a specific ROM (`PKMNG5PC_ROM` also works) |
| `--runner <path>` | use a specific runner (`PKMNG5PC_RUNNER` also works) |
| `--banks <dir>` | check that title banks exist in `<dir>` instead of `banks/black` (`-Banks` in PowerShell); should match the directory the runner was configured with |
| `--layout stacked\|separate` | one top-over-bottom window (default) or two windows |
| `--native` | present the two screens through this project's own `render/` compositor (Vulkan) instead of the runner's SDL blit — see [Native present](#native-present) |
| `--frames N` | present N frames, print fps and a frame hash, exit — a self-test |
| `--stats` | print the performance summary on exit |
| `--sav [<file>]` | keep the battery save at another path; the default is the ROM's own `<rom>.sav`. Aliases: `--save-path`, `--save`; `--no-save` (`-NoSave` in PowerShell) runs a blank cart and writes nothing |

Controls: `arrows` = D-pad, `Z`/`X` = A/B, `A`/`S` = Y/X, `Q`/`W` = L/R, `Enter` = Start, `Backspace` = Select, `Esc` = quit. The bottom-screen window is the touchscreen.

#### Native present

`--native` (`-Native` in PowerShell) is the first wiring of `Plan.md` M8's own
present/compositor: instead of the runner's fixed 512×768 top-over-bottom SDL
blit, the two DS screens go to `pkmn::render::Presenter` (`render/`, Vulkan),
composed by `compose_screens_bgra8()`. It sets `PKMNG5PC_PRESENT=native`, the
switch the frontend hook reads. The run is otherwise identical — same guest
execution, input, audio and save behaviour, and the same presented-frame digest,
so `--frames N --stats` on either path reports the same `frame_fnv`. Pointer
input is mapped through the same layout rule that composed the frame, so the
bottom screen is clickable exactly where it is drawn.

It needs a runner built with `PKMNG5PC_NATIVE_PRESENT=ON` (the default when
`PKMNG5PC_BUILD_RUNNER=ON`), which adds the port's only modification to the
pinned `ndsrecomp` submodule: a six-site, guarded hook in the vendored
`runner/src/frontend.cpp`, carried as
`patches/ndsrecomp/0001-native-present-hook.patch` and applied by configure
(idempotently — reverting the submodule with `git -C third_party/ndsrecomp
checkout .` and re-configuring re-applies it). Build with
`-DPKMNG5PC_NATIVE_PRESENT=OFF` for a completely untouched submodule.
`docs/adr/0006-ndsrecomp-native-present-hook.md` records the decision.

Because the presenter owns one output surface, `--native` is **stacked only**:
`--layout separate` is ignored with a warning. Pointer and touch input work:
the compositor's placement rule is inverted to turn a window click into a
stylus coordinate, and a click outside the bottom screen (top screen or
letterbox) does not touch. Window layout, post-FX and frame decoupling are the
rest of M8.

The underlying invocation, if you want to drive it yourself (`tools/play` sets `NDS_GPU2D_THREADED=0` for you — see the workaround note below):

```sh
NDS_GPU2D_THREADED=0 \
build/windows-relwithdebinfo/third_party/ndsrecomp/runner/nds_runner.exe \
    bios 250000000 --boot direct --generated-firmware --freebios \
    --rom "roms/<your-rom>.nds" \
    --interactive
```

- `NDS_GPU2D_THREADED=0` puts the 2D raster on the emulation thread. The default threaded scanline path wedges one scheduler round at ≈83.69 s of emulated time (vendored `gpu2d` defect; the two paths are documented as the same latch/execute sequence), so all long runs set it. This trades some wall time for correctness, not pixels.

- The two **positional** arguments are the BIOS directory (a scratch dir; the runner writes a generated Wi-Fi identity into it) then the **ARM9 cycle budget**. Under `--interactive` the budget is parsed but **inert**: the window stays open until you close it or press `Esc`. Without `--interactive` the run executes the budget and exits with a CPU and graphics report instead of opening a window.
- `--interactive` also starts the debug server on port 19842 (`debug_pump_start`), so `tools/ndsquery.py` can query a running window — but execution belongs to the frontend, so the `run_to_*` commands are rejected. Use `--serve` for a headless run you drive step by step.
- `--freebios` supplies the BSD-2 BIOS images vendored inside `ndsrecomp`; it is effectively required, since without it the runner wants retail BIOS dumps and aborts with `dump verification failed`. Both `--freebios` and `--generated-firmware` require `--boot direct`.
- `--save-path <file>` loads a battery save at boot and persists it; `--no-save` runs with a blank cartridge and writes nothing. Either way `tools/play` passes `--config re/config/frontend.toml` (the `[cartridge]` table): Gen 5 carts carry 512 KiB of flash, and without it the runner's historical 8 KiB EEPROM fallback makes the game's own save init fail (it also rejects a 512 KiB `.sav` as an invalid size).
- Diagnostics: `--diagnostics on --diagnostics-dir <dir>` writes a performance bundle, and a coverage manifest is written next to the ROM by default (`tools/play` passes `--no-coverage-manifest`).
- For automated eyes, `--serve` starts the same run headlessly with the JSON debug server on port 19842 (`third_party/ndsrecomp/TCP.md`), and `tools/runwatch.py` drives it: it logs a timeline of vblank counts, ARM9/ARM7 PCs, DISPCNT for both engines and a hash per frame, writes a PNG plus a console-layout (top over bottom) montage whenever the screen changes, and — if the watched counter stops advancing — takes a stall report instead of waiting out the protocol's round cap.

  ```sh
  python tools/runwatch.py --runner build/windows-relwithdebinfo/third_party/ndsrecomp/runner/nds_runner.exe \
      --rom "roms/<your-rom>.nds" --screenshots banks/screenshots --out banks/timeline.jsonl
  ```

  Screenshots are ROM-derived output: keep them in the git-ignored `banks/` tree.

#### Saves

`tools/play` starts on the battery save and keeps it updated, so a run continues where the cartridge left off instead of always showing NEW GAME. Saves are **on by default**: the file is the ROM's own `<rom>.sav`, created on the first in-game save.

```sh
# just play - this loads and keeps roms/<your-rom>.sav
pwsh tools/play.ps1
sh tools/play.sh

# a file you keep elsewhere (e.g. one exported from your own DS)
pwsh tools/play.ps1 -Sav "banks/saves/black.sav"
sh tools/play.sh --sav banks/saves/black.sav

# write nothing: a blank 512 KiB cart in RAM
pwsh tools/play.ps1 -NoSave
sh tools/play.sh --no-save
```

A Gen 5 save on disk is the cartridge's **512 KiB flash image**; the game's own 0x24000-byte save (rotating blocks + checksums) lives inside it. That is why every `tools/play` run passes `--config re/config/frontend.toml`, which declares the `[cartridge]` chip as 512 KiB flash — without it the runner's historical 8 KiB EEPROM fallback rejects any 512 KiB file as an invalid size and the game's save init fails. The file has to be that full image; a tool that exports the raw backup chip (`.sav`, or an emulator's `.dsv`) produces one. The runner does not validate the save's contents — the game does — and it logs what it did (`[save] loaded 524288 bytes from …`), while `tools/play` prints the file it used as `save:` in its header.

Persistence is **on by default**; `--no-save` (`-NoSave`) turns it off and leaves the same 512 KiB flash cartridge blank with no file behind it. If the save file does not exist yet you get a fresh cartridge that is written on the first in-game save. `-Save` / `--save` (no value) is the older spelling of `-Sav ""` / `--sav ""`, and `--save-path <file>` still works.

Loading a save is also **how a session continues**: with one loaded, Black boots past the menu into the game and is playable, and the game's own saving round-trips to the file. The same save-capable configuration — the 512 KiB flash cartridge that `re/config/frontend.toml` declares — is what **NEW GAME** needs as well, with or without a save file attached; a run that leaves the cartridge at the runner's 8 KiB EEPROM default (a hand-rolled `nds_runner` invocation with no `--config`) still crashes on NEW GAME, and `tools/play` never does that.

Saves are game data, so they stay out of the repo (`roms/` and `banks/` are git-ignored) — and because the runner rewrites the file in place whenever the game saves, keep your own copy of a save you care about.

Status: the pipeline is verified end-to-end — banks generate, link, and register against the ROM SHA-1 at run time, both CPUs execute, and the game boots from your ROM all the way to its **navigable main menu**. **All four titles boot to their intro and 3D title screen** (White, Black 2 and White 2 were played through the intro and watched on their title screens on a live window on 2026-10-16, each with its own bank group in the same runner); Black is the title driven all the way through the menu. `tools/play` opens the live SDL window: the top screen shows the Pokémon Company + Nintendo boot splash, the intro cinematic (Game Freak logo, castle/throne, Zekrom, cast), the title screen, and then the four-entry startup menu — NEW GAME / MYSTERY GIFT / NINTENDO WFC SETTINGS / MIC TEST — after an **A** press (Start does not advance the title). The selection bar moves under the D-pad, and A opens sub-screens (e.g. MYSTERY GIFT's own menu, with "Welcome to Mystery Gift!" on the bottom screen). This is **M2.5's gate, met 2026-10-16 on a live window** (Windows; Linux unverified). Presented frames now run at the audio-paced ceiling — **59.7–60 fps** on all four titles over a 1200-frame boot window (measured 2026-10-19, from the frontend's own soak counters) — where the ARM7/ARM9 interpreter once held them to 45–50. **Added 2026-10-18: the port reaches gameplay.** With a battery save loaded (see [Saves](#saves)) Black boots past the menu into a playable game and the game's own saving round-trips to the 512 KiB file; **NEW GAME reaches the game on the same save-capable build too** (the cartridge declared as 512 KiB flash, save file or not), while a run that leaves the cartridge at the runner's 8 KiB EEPROM default (a hand-rolled invocation with no `--config`) still crashes on NEW GAME. The maintainer then played a NEW GAME by hand into Nuvema Town — walking, area transitions, text boxes, the starter choice, trainer battles — and `Plan.md` M3 ticks its five overworld systems on the strength of it (they are the recompiled game's own code plus the runner's HAL). Evidence: `re/notes/recompilation.md`.

Three known limitations remain:

- The vendored runner's threaded 2D path wedges one scheduler round at ≈83.69 s of emulated time. `tools/play` works around it with `NDS_GPU2D_THREADED=0` (see the invocation note above).
- **Most overlays are not banked.** Gen 5 keeps most of its code in the ARM9 overlays — 237 files (B/W) / 344 (B2/W2), 3.26 / 3.69 MB, roughly 5x the decoded ARM9 module. `pkmn_bankgen --overlays` (step 3) banks the overlays a coverage ingest has observed — the four titles currently carry seeds for a handful each — but the rest still runs through the runner's runtime live-overlay path, which is off by default. So in-game menus and battles still interpret, and this is the largest remaining performance item. The ARM9 *module* itself is fully seeded on all four titles as of 2026-10-19 (Black 8422 functions, White 8438, Black 2 9553, White 2 9464), which took the boot window from ~101–105 k interpreted ARM9 instructions per frame to ~640, `tier3_arm9` from ~21 ms to **0.03–0.06 ms/frame**, and the presented rate from ~33 to the audio-paced **~59.7 fps** — see `re/notes/recompilation.md` §"The ARM9 ingest lands on all four titles".
- A run that reaches the runner without the cartridge table — no `--config re/config/frontend.toml` — keeps the runner's 8 KiB EEPROM fallback and crashes on NEW GAME. `tools/play` always passes it, so this only affects hand-rolled invocations. See [Saves](#saves).

The measured coverage baseline behind those numbers (bank sizes, what is banked vs not, per-frame counters, and what the graphics path costs) is in `re/notes/recompilation.md` §"What is recompiled, and what still costs".

See `re/notes/recompilation.md` and the M2.3/M2.5/M3 entries in `Plan.md`.

### Troubleshooting

- `refusing to start: dump verification failed` — add `--freebios`.
- `[save] ignoring <file>: expected 8192 bytes, found 524288` — the run has no cartridge config, so the runner is still using its 8 KiB EEPROM fallback. `tools/play` always passes `--config re/config/frontend.toml`; a hand-rolled invocation has to pass it too; see [Saves](#saves).
- `PKMNG5PC_BUILD_RUNNER=ON but no generated title banks were found` — run step 3 first; check `PKMNG5PC_BANKS_DIR` if the banks are somewhere else.
- Two `ndsrecomp` tests are environment-sensitive on Windows and can fail without affecting the runner: `gpu2d_window_test` overflows the default thread stack (`0xC00000FD`; the `nds_runner` target gets a 64 MB stack, the test does not), and `live_index_lock_test` (Python-driven) is intermittent — its index writers sometimes fail to spawn.

## ROMs (required for the ROM-dependent tests)

Place your own legally obtained ROMs in `roms/` (git-ignored). Any dump filename works, e.g.:

```
roms/<your-rom>.nds
```

Supported titles are the USA/EU NDSi-Enhanced dumps of Black, White, Black 2 and White 2. All four boot to their intro and 3D title screen today (Black is verified further, to the main menu). Dumps are referred to by path only; no dump's release filename is recorded in this repo.

No ROM is ever committed, and no ROM is required to build or for CI.

## Testing

There are two layers of tests:

1. **ROM-independent (pure unit tests)** — compression codecs and hash primitives. These always run and are what CI executes (CI has no ROMs).
2. **ROM-dependent** — NitroFS/overlay/NARC parsing against a real ROM. If no ROM is found, the test prints `(skipped: no ROM found; set PKMNG5PC_ROM)` and passes, so the suite stays green without a ROM.

### How a ROM is located

The ROM test resolves the ROM in this order:

1. The `PKMNG5PC_ROM` environment variable, if set to a `.nds` path.
2. Otherwise, the first `*.nds` file found in `roms/`.

To test a specific ROM, set the variable **before** running:

```sh
# Windows (PowerShell)
$env:PKMNG5PC_ROM = "C:\path\to\your-rom.nds"
ctest --preset windows-debug --output-on-failure

# Linux / macOS
PKMNG5PC_ROM="/path/to/black.nds" ctest --preset linux-debug --output-on-failure
```

### Run everything

```sh
ctest --preset windows-debug --output-on-failure
```

### Run the test binary directly (see per-test detail)

`ctest` hides stdout on success. To see the ROM inventory, NARC counts, overlay matches, etc., run the binary directly:

```sh
# Windows
build/windows-debug/tests/pkmn_tests.exe

# Linux
./build/linux-debug/tests/pkmn_tests.exe
```

The binary accepts an optional **name filter** as its first argument (substring match), e.g.:

```sh
build/windows-debug/tests/pkmn_tests.exe lz11       # only runs lz11_roundtrip
build/windows-debug/tests/pkmn_tests.exe nds        # only runs nds_overlays_decompress
```

### What the tests cover

Compression round-trips (`tests/compression_test.cpp`):

- `rle_roundtrip`, `lz10_roundtrip`, `lz11_roundtrip`, `huffman4_roundtrip`, `huffman8_roundtrip` — encode then decode and compare.
- `decompress_rejects_bad_input` — malformed input throws instead of crashing.

ROM inventory (`tests/rom_overlay_test.cpp`):

- `nds_overlays_decompress` — parses the header, decompresses the ARM9 (BLZ), walks the NitroFS FNT/FAT tree, sniffs NARC archives, and BLZ-decompresses every ARM9 overlay, checking each against its declared RAM size.

Extraction (`tests/extract_test.cpp`):

- `extract_manifest_deterministic` — extracts a subset twice and re-verifies every hash and size.

Hashes (`tests/sha1_test.cpp`, `tests/sha256_test.cpp`):

- `sha1_known_vectors`, `sha256_known_vectors` — published test vectors.
- `sha1_padding_boundaries` — lengths that straddle SHA-1's padding rules, cross-checked against an independent implementation.

### Expected output (example)

```
[ RUN  ] rle_roundtrip
[ PASS ] rle_roundtrip
...
  title='POKEMON B' code='IRBO' arm9=456856 ovl9=7584
  arm9 blz: 456856 -> 681920
  nitrofs: 247 files
  narc: 237 archives, 54054 members, 1 named, 0 bad
  overlays: 229 compressed (229 ram-matched), 8 raw matched, 0 mismatched
[ PASS ] nds_overlays_decompress

11 test(s) run, 0 failure(s)
```

A healthy run shows `0 mismatched`, `0 bad`, and no `FAIL` lines.

## Tools

`pkmn_romdump` inspects a ROM without running the game. Build first, then:

```sh
build/windows-debug/tools/pkmn_romdump.exe "roms/<your rom>.nds" [options]

# Options
--list              # print the NitroFS file tree (id, size, path)
--narcs             # list every NARC archive, its member count and names
--overlays          # list ARM9 overlays and BLZ decode sizes
--extract <dir>     # write a full extracted/ inventory + SHA-256 manifest
--narc-limit <n>    # with --extract: expand only the first n NARC archives
```

Options can be combined (e.g. `--list --overlays`). With no options it prints the header (title, game code, ARM9/overlay/FNT/FAT offsets) and the decompressed ARM9 size.

`pkmn_bankgen` generates the runner's title banks from your ROM — see [Runner](#runner-the-bootable-build).

`tools/play.ps1` / `tools/play.sh` open the live window — see [Run](#5-run). They are the "just show me the game" entry point; the two positional runner arguments, the boot flags and the ROM choice are all filled in for you.

`tools/runwatch.py` is the bring-up harness. It launches the runner with `--serve` and samples the running game over the debug protocol, so one long run produces a progress curve (emulated time, ARM9/ARM7 PCs, VCOUNT/DISPSTAT, DISPCNT enables, frame hashes) rather than a single endpoint. It advances on **scheduler rounds** by default, because the protocol's natural driver is not safe: `run_to_event vblank9` stalls whenever the guest disables the VBlank IRQ (the counter is only bumped while DISPSTAT bit 3 is set), and `run_cycles` stops on a halted CPU. `--drive cycles|vblank9` selects those explicitly. `--screenshots` writes each change as a PNG plus a top-over-bottom montage, and a frozen raster produces a stall report (interrupt state, halt reasons, raw DISPSTAT, a controlled probe) rather than a silent hang. Requires only Python 3; writes to the git-ignored `banks/` tree by default. `tools/ndsquery.py` sends ad-hoc one-shot commands to a running server.

### Extracting a ROM

```sh
build/windows-debug/tools/pkmn_romdump.exe "roms/<your rom>.nds" --extract extracted/black
```

This writes (all git-ignored under `extracted/`):

```
extracted/black/arm9.bin             decoded ARM9
extracted/black/overlays/ovNNN.bin   decoded ARM9 overlays
extracted/black/nitrofs/<path>       named NitroFS files (raw)
extracted/black/narc/<archive>/<id>  expanded NARC members
extracted/black/manifest.sha256      "<sha256>  <path>" for every file
```

The manifest uses the standard `sha256sum` format and is deterministic across runs; verify it with `sha256sum -c manifest.sha256` (Linux) from inside the output directory. Full extraction writes tens of thousands of files and takes a couple of minutes; use `--narc-limit <n>` for a quick partial run.

## Formatting / lint

The project uses clang-format (`.clang-format`, WebKit-based). Before finishing any change, check that your source files conform (clang-format does not recurse directories, so list the files):

```sh
# Linux / macOS
clang-format --dry-run --Werror $(find rom tools tests \( -name '*.cpp' -o -name '*.h' \))

# Windows (PowerShell)
clang-format --dry-run --Werror (Get-ChildItem rom,tools,tests -Recurse -Include *.cpp,*.h).FullName

# Apply the fixes in place (same file list, replace -i for --dry-run --Werror)
clang-format -i rom/src/nds.cpp
```

## Credits

This port is a thin layer of our own work standing on a lot of other people's. With thanks to:

- **[ndsrecomp](https://github.com/RetroPortingToolKit/ndsrecomp)** — the static recompiler and runner the game actually executes on (ARM9/ARM7 → C++), including the melonDS-derived 3D geometry engine (software + compute) and Wi-Fi device model.
- **[melonDS](https://github.com/melonDS-emu/melonDS)** — the reference DS emulator those ndsrecomp subtrees come from, and the oracle this port compares frames, memory and saves against; **[DeSmuME](https://github.com/TASEmulators/desmume)** in the same role.
- **[arm-recomp-core](https://github.com/mstan/arm-recomp-core)** and **[freebios](https://github.com/mstan/freebios)** (mstan) — the ARM→C recompilation core, and the BSD-2 BIOS replacement that makes a ROM-only boot possible.
- **[Ghidra](https://github.com/NationalSecurityAgency/ghidra)**, **[Tinke](https://github.com/pleonex/tinke)** and **[DSPRE](https://github.com/MarcRobler/DSPRE)** — the reverse-engineering and Nitro/Gen-IV–V format tooling behind `re/`, alongside **[ds-decomp](https://github.com/AetiasHax/ds-decomp)** (delink/Ghidra/objdiff) and **[GBATEK](https://problemkaputt.de/gbatek.htm)** for the hardware.
- The **decompilation community** — the `pret` projects and the early Gen 5 efforts that map this generation's formats and data.
- **SDL3** and **toml++** — windowing/input/audio and configuration.

Licenses and full attribution are in [`NOTICE`](NOTICE). None of the above is redistributed as game content; see [Legal](#legal).

## Legal

The program ships without Nintendo/Game Freak assets and requires the user's own ROMs at run time. See `NOTICE` and `AGENTS.md` section 2.
