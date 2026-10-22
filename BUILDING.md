# Building & Running — Flags and Options

Quick reference for every build and run flag. Windows examples use `pwsh`; the
`.sh` mirror of each tool takes the long-form equivalent (noted per section).

Run from the repository root.

---

## 1. Bootstrap the recompiler

Builds the pinned `ndsrecomp` submodule, stages toml++, and generates the BIOS
banks.

```sh
pwsh tools/bootstrap_ndsrecomp.ps1            # Windows
sh tools/bootstrap_ndsrecomp.sh               # Linux / macOS
```

### Flags

| Windows (`bootstrap_ndsrecomp.ps1`) | Linux/macOS (`bootstrap_ndsrecomp.sh`) | Effect |
| --- | --- | --- |
| `-BuildRunner` | `--build-runner` | Also configure + build a standalone runner under `third_party/ndsrecomp/runner/build/`. Not needed for the normal flow — step 4 builds the runner as part of this project. |
| `-Sdl3` | `--sdl3` | With `-BuildRunner`: build the runner against SDL3 (`NDS_SDL_BACKEND=SDL3`, compute renderer ON). Without it the standalone runner is headless (`NONE`, compute OFF). |
| `-Configuration <cfg>` | *(not exposed; fixed `Release`)* | CMake build type for the recompiler/runner (`Release`, `Debug`, `RelWithDebInfo`). |
| `-Compiler <cc>` | `--compiler <cc>` | Toolchain for the recompiler (and the runner, with `-BuildRunner`). See below. |

### `-Compiler` / `--compiler`

- A **bare name** resolved from `PATH` (`clang`, `clang-18`, `gcc`), or a
  **path** to the C compiler (`C:/tools/llvm-mingw/bin/clang.exe`).
- The C++ sibling is **derived**: `clang` → `clang++`, `gcc` → `g++`, `cl` → `cl`.
  Version suffixes carry over (`clang-18` → `clang++-18`). For a path, the C++
  compiler must sit in the same directory.
- Anything it can't derive is **refused**, not guessed (a cross-compiled
  `x86_64-w64-mingw32-gcc` is rejected).
- A nonexistent path, or a path whose C++ sibling is missing, is refused.

```sh
pwsh tools/bootstrap_ndsrecomp.ps1 -Compiler clang
pwsh tools/bootstrap_ndsrecomp.ps1 -Compiler C:/tools/llvm-mingw/bin/clang.exe
sh tools/bootstrap_ndsrecomp.sh --compiler clang
```

> CMake caches the compiler. Switching to a different compiler on an existing
> build dir is **refused**; remove `third_party/ndsrecomp/recompiler/build`
> (and `runner/build`) first.

---

## 2. Build this project (presets)

```sh
cmake --preset <preset>              # configure
cmake --build --preset <preset>      # build
ctest --preset <preset>              # run tests
```

### Configure / build / test presets

| Preset | Build type | Compiler pin |
| --- | --- | --- |
| `windows-debug` | `Debug` | `clang` / `clang++` |
| `windows-release` | `Release` | `clang` / `clang++` |
| `windows-relwithdebinfo` | `RelWithDebInfo` | `clang` / `clang++` |
| `linux-debug` | `Debug` | *(default toolchain)* |
| `linux-release` | `Release` | *(default toolchain)* |
| `linux-relwithdebinfo` | `RelWithDebInfo` | *(default toolchain)* |

Build dir: `build/<preset>/`.

Override the compiler on any configure with
`-DCMAKE_C_COMPILER=... -DCMAKE_CXX_COMPILER=...`. Changing it on an existing
build dir needs a fresh `build/<preset>/`.

### Project cache variables (pass to `cmake --preset`)

| Variable | Default | Effect |
| --- | --- | --- |
| `PKMNG5PC_BUILD_TESTS` | `ON` | Build unit tests (`tests/`). |
| `PKMNG5PC_BUILD_TOOLS` | `ON` | Build local extraction tools (`tools/`, incl. `pkmn_bankgen`). |
| `PKMNG5PC_BUILD_RENDER` | `ON` | Build the render seam and GPU backends (`render/`). |
| `PKMNG5PC_BUILD_RUNNER` | `OFF` | Build the ndsrecomp runner and fetch/build its SDL3. Required before `tools/play`. |
| `PKMNG5PC_NATIVE_PRESENT` | `ON` | Let the runner present through `render/` (Vulkan). Needs `PKMNG5PC_BUILD_RENDER=ON`. Turning it OFF builds a runner with no native present path (so `tools/play -Native` won't work). |
| `PKMNG5PC_NATIVE_RENDERER` | `OFF` | Build the native DS render sinks (alpha). Needs `PKMNG5PC_BUILD_RENDER=ON` and Vulkan, plus `PKMNG5PC_NATIVE_PRESENT=ON` whenever `PKMNG5PC_BUILD_RUNNER=ON`. The runtime switch (`PKMNG5PC_RENDERER`) stays off by default. |
| `PKMNG5PC_BANKS_DIR` | `banks/black` | Bank directory compiled into the runner. |
| `PKMNG5PC_TITLE_ROM_SHA1` | `26ad0b9967aa279c4a266ee69f52b9b2332399a5` (Black) | ROM SHA-1 the runner registers the bank group under. |

Runner-only variables are set internally (`cmake/Runner.cmake`) and are **not**
user-settable: `NDS_BOOTSTRAP_FIRMWARE=ON`, `NDS_SDL_BACKEND=SDL3`,
`NDS_ENABLE_COMPUTE_RENDERER=OFF`, `NDS_TITLE_BANK_DIR`, `NDS_TITLE_ROM_SHA1`.

### Typical runner configure

```sh
cmake --preset windows-relwithdebinfo -DPKMNG5PC_BUILD_RUNNER=ON
cmake --build --preset windows-relwithdebinfo
```

Configure applies the tracked patches in `patches/ndsrecomp/` in filename order
and idempotently whenever the runner or native renderer is enabled, so the
submodule shows as modified by design. `0003` installs
on top of `0001`'s guarded present hook and must stay after it; `0004` adds the
guarded `GPU3D` device accessor; `0005` makes the vendored melonDS Wi-Fi
sources use `_WIN32` and libslirp's `slirp_ssize_t` so plain LLVM Clang
targeting MSVC/UCRT builds them. Revert with
`git -C third_party/ndsrecomp checkout .` before switching revisions.

---

## 3. Generate the banks (`pkmn_bankgen`)

```sh
build/<preset>/tools/pkmn_bankgen <rom.nds> <out-dir> [options]
```

| Option | Effect |
| --- | --- |
| `--recompiler <path>` | Path to `nds_recompile`. Without it, bankgen only emits (no recompile). |
| `--title-config <toml>` | Title config, e.g. `re/config/black.toml`, `re/config/black2.toml`. |
| `--shards <n>` | Shard count; must be `>= 2` (the runner globs `*_NN.c`). |
| `--emit-only` | Emit banks without running the recompiler. |
| `--overlays` | Add one content-validated ARM9 bank per overlay with an observed seed. |
| `--overlay-seeds <dir>` | Seed directory (required with `--overlays`). |

The generated `<out-dir>` is what to pass as `PKMNG5PC_BANKS_DIR`.

---

## 4. Run the game (`tools/play`)

```sh
pwsh tools/play.ps1               # Windows
sh tools/play.sh                  # Linux / macOS
```

### Flags

| Windows (`play.ps1`) | Linux/macOS (`play.sh`) | Effect |
| --- | --- | --- |
| `-Rom <path>` | `--rom <path>` | ROM to run. Default: the Black image (`IRBO`) in `roms/`, or the only `.nds` present. Env: `PKMNG5PC_ROM`. |
| `-Runner <path>` | `--runner <path>` | Runner executable. Otherwise auto-found under `build/<preset>/.../nds_runner`. Env: `PKMNG5PC_RUNNER`. |
| `-BiosDir <dir>` | `--bios-dir <dir>` | BIOS directory. Default `bios`. |
| `-Banks <dir>` | `--banks <dir>` | Bank directory to pre-check. Default `banks/black`. Pass the same dir the runner was configured with. |
| `-Layout <stacked\|separate>` | `--layout <stacked\|separate>` | Screen layout for the SDL present. Default `stacked`. |
| `-Native` | `--native` | Present through `render/` (Vulkan), single window, always stacked. Needs a runner built with `PKMNG5PC_NATIVE_PRESENT=ON`. |
| `-Frames <n>` | `--frames <n>` | Present `n` frames then exit. `0` = run until closed. Sets `NDS_FRONTEND_MAX_FRAMES`. |
| `-Sav <path>` / `-SavePath <path>` | `--sav <path>` / `--save-path <path>` | Save file to use. Default: `<rom>.sav`. |
| `-Save` | `--save` | Use the default save path explicitly (empty value = `<rom>.sav`). |
| `-NoSave` | `--no-save` | Blank 512 KiB cart, keep everything in RAM, write nothing. |
| `-Stats` | `--stats` | Print run statistics. Sets `NDS_FRONTEND_STATS=1`. |
| `-Coverage` | `--coverage` | Keep the coverage manifest (default: `--no-coverage-manifest`). |
| `-Diagnostics` | `--diagnostics` | Keep a perf bundle in `banks/play/` (`--diagnostics on`). |
| `-Extra <args...>` | `-- <args...>` | Pass extra arguments straight to the runner. |

### Flags combinations and side effects

- `-NoSave` **cannot** combine with `-Sav`/`-Save`.
- `-Native` forces `-Layout` to `stacked` (single output surface); a differing
  layout is ignored with a warning.
- `-Frames > 0` implies `-Stats`.
- Saves are on by default: load `<rom>.sav`, start blank if missing, keep
  writing. The run always passes `re/config/frontend.toml` (declares the
  512 KiB Gen 5 cart).
- Environment defaults set by `tools/play` (override by setting them yourself):
  - `NDS_FRONTEND_MAX_FRAMES` — from `-Frames`.
  - `NDS_FRONTEND_STATS` — from `-Stats` or `-Frames`.
  - `NDS_GPU2D_THREADED=0` — the inline 2D scanline path (the threaded one
    wedges at t=83.69 s emulated).
  - `PKMNG5PC_PRESENT=native` — when `-Native`.

### In-window keys

| Key | Action |
| --- | --- |
| Arrows | D-pad |
| `Z` / `X` | A / B |
| `A` / `S` | Y / X |
| `Q` / `W` | L / R |
| Enter | Start |
| Backspace | Select |
| Esc | Quit |
