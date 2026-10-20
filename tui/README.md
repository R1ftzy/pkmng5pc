# Terminal launcher

A dependency-free terminal UI for the local pkmng5pc workflow. It wraps the
existing CMake presets, `tools/` scripts and `pkmn_bankgen`; it does not emulate
the game or ship any game content.

## Status

Complete and verified locally on Windows. The launcher is pure Python, so it
starts without the C++ build; `--check` and `--snapshot` also work without an
interactive terminal.

- `python -m unittest discover -s tui/tests` runs 64 tests (one
  platform-specific case is skipped).
- The Windows native console reader was exercised with injected mouse records:
  left press, release and wheel-up decoded correctly through
  `ReadConsoleInputW`. A real hidden console also launched the TUI, switched to
  the ROM page from a native mouse click, and exited through native key input.
- 72 page snapshots — six pages at 60×18, 100×34 and 140×50, with color on/off
  and Unicode/ASCII glyphs — render without line overflow. 36 final-review
  renders were checked at the same sizes and modes, scrolled to the top, middle
  and bottom.
- The recompilation panel was checked against the local Black bank set: 11,989
  emitted functions, 24,553 interpreter entries, 134 distinct targets and
  0.09% interpreter for the paired coverage/performance run. The panel also
  reported the older coverage as predating the current bank headers. Generated
  banks, manifests and logs stay local and are never committed.
- `python tui/launcher.py --check` detects the local presets, ROM folder, tools,
  banks and runner, and exits 0 when a readable ROM is present.

## Requirements

- Python 3.10 or newer. No third-party packages, curses or Textual.
- An interactive ANSI terminal (Windows Terminal / PowerShell, or a Unix pty).
- CMake 3.24+ with `cmake` and `ctest` on `PATH`.
- The toolchain used by the selected preset (for the Windows presets, `clang`).
- A legally obtained ROM, in `roms/` or anywhere else you can browse to.

Run everything from the repository root.

## Start

```sh
python tui/launcher.py
```

Useful options:

```text
--rom PATH          preselect a ROM for this session
--preset NAME       preselect a build preset
--state PATH        alternate settings file (default: tui/.state.json)
--no-color          disable ANSI colors
--ascii             use ASCII glyphs only
--snapshot [PAGE]   render one page and exit (home, rom, build, play, setup, help)
--width N           snapshot width (default 100)
--height N          snapshot height (default 32)
--check             print detected tools, ROMs, banks and runner, then exit
```

Examples:

```sh
python tui/launcher.py --check
python tui/launcher.py --snapshot build --no-color --width 100 --height 34
```

`--check` is the quickest way to diagnose a missing `cmake`, `pkmn_bankgen`,
runner or recompiler. The interactive launcher needs a TTY; in a pipe or CI use
`--snapshot` or `--check` instead.

## Tests

```sh
python -m unittest discover -s tui/tests
```

The tests use synthetic in-memory ROM headers, temporary folders and a local
Python subprocess; no ROM or game content is needed.

## Screens

### Home (1)

Status summary and quick actions: Quickstart, Play now, Configure & build,
Choose ROM, Generate title banks and Build settings. The first line names the
next missing step for a full play setup.

The **RECOMPILATION** block shows emitted function counts (ARM9/ARM7), the
runtime native/interpreter split, interpreter coverage (entries, distinct
fallback targets, instructions) and the evidence build/time. Missing,
mismatched or stale evidence is labeled instead of estimated.

**Quickstart** fills the gaps automatically: bootstrap the recompiler when a
runner rebuild needs it, build `pkmn_bankgen` if needed, generate missing title
banks, build the runner when needed, then play. It only adds the steps that are
actually missing.
It may point Banks directory at the generated output, enable Build host tools,
Build render and Build runner, and disable Emit only so the run is playable;
cancelling its final review restores those settings. Every run is reviewed
before it starts.

### ROM (2)

- **ROM folder** opens the folder browser. Enter opens a folder, `S` chooses the
  current folder, `G` lets you type a path, `H` toggles hidden entries.
- **Rescan folder** re-reads `.nds`/`.srl` headers.
- **Choose ROM file** browses for a ROM and previews title, game code and size.
- **Selected ROM** shows the header fields, path, save file and full-ROM SHA-1.
- **Detected ROMs** lists every `.nds`/`.srl` in the folder; Enter selects one.

The SHA-1 is computed in a background thread and passed to Configure as
`PKMNG5PC_TITLE_ROM_SHA1`, so the runner registers the matching bank group.

### Build (3)

- Human-readable profile selection (`windows-*` on Windows, `linux-*` on Unix),
  with build type, compiler, description, recommended state and the exact CMake
  preset ID. The ID remains the value used by the commands.
- CMake toggles: unit tests, host tools, render layer, ndsrecomp runner and
  native present. Passed explicitly on every Configure.
- Banks directory, bank-set status and the auto-detected runner title SHA-1.
- Detailed recompilation state: function totals, dispatch rows, runtime
  interpreter share, interpreter coverage and evidence freshness.
- Multi-title advanced fields for `NDS_TITLE_BANK_DIRS` and
  `NDS_TITLE_ROM_SHA1S` (semicolon separated; leave blank to keep the cache).
- Actions: Configure, Build, Configure & build and Run tests. Each action opens
  the final review with the exact command, working directory, notes and every
  current configuration value.
- Build state shows whether the options match the existing CMake cache.

### Play (4)

- Screen layout: `stacked` (one window) or `separate` (two windows).
- Native present through `render/` (Vulkan); it forces the stacked layout.
- Frame limit (`0` runs until the window is closed).
- Save mode: default beside the ROM, a custom file, or off (blank cart).
- Diagnostics: stats, coverage manifest and a performance bundle. Coverage is
  written beside the ROM; the performance bundle goes to `banks/play`. Both feed
  the RECOMPILATION panel.
- Extra runner arguments, runner/bank status and Play.

Play always passes the discovered runner and the selected bank directory
explicitly. The game window owns keyboard focus while it is open; close it to
return to the log. With diagnostics off, the panel reports runtime state as
**not measured** rather than showing an old or guessed value.

### Setup (5)

- Bootstrap the recompiler, optionally with the standalone runner, SDL3 and a
  specific C compiler.
- Generate title banks from the selected ROM: output directory, title config,
  shard count, overlays/seed directory and emit-only mode.
- **Use output as build banks dir** points the Build page at the new output.

### Help (6)

Keys, the recommended workflow, recompilation-metric definitions and the
local-state/legal notes.

## Keys

| Context | Keys |
| --- | --- |
| Pages | `Up`/`Down` move, `Left`/`Right` change, `Enter` activate, `Space` toggle |
| Pages | `Tab`/`Shift+Tab` next/previous page, `1`-`6` jump, `H` `R` `B` `P` `S` page shortcuts |
| Pages | `?` help, `L` last log, `Q` quit |
| Mouse | Click a tab, row or button; wheel scrolls pages, reviews, the browser and the job log |
| Text input | Type, `Backspace`/`Delete`, `Left`/`Right`, `Home`/`End`, `Ctrl+U` clear, `Enter` accept, `Esc` cancel |
| Folder/ROM browser | `Up`/`Down`, `Enter` open or pick, `Backspace`/`Left` up, `Right` open, `S` use folder, `G` go to path, `H` hidden, `R` rescan, `Esc` cancel |
| Folder/ROM browser (mouse) | Click an entry to select it; click the selected entry to open or pick it; wheel moves three entries |
| Final review | `Up`/`Down`/`PgUp`/`PgDn`/`Home`/`End` scroll, `Enter` run, `Esc` cancel; `Run`/`Cancel` are clickable |
| Job log | `Esc`/`Ctrl+C` cancel while running; `Up`/`Down`/`PgUp`/`PgDn`/`Home` scroll; `End`/`F` follow; `Enter` return after it finishes |

## Recompilation state

Home, Build and the final review read the same local evidence. The launcher does
not invent a completion percentage:

- **Recompiled functions** is the sum of the `Total functions` marker in the
  selected banks directory's `*_arm9_*.h` / `*_arm7_*.h` headers. It is static
  recompiler output, not a completion estimate.
- **Dispatch rows** is the sum of the generated `g_dispatch_*_len` arrays the
  runner binary-searches when control transfers between recompiled code.
- **Runtime evidence** pairs the latest coverage manifest for the selected
  ROM's SHA-1 with the `performance-*.jsonl` log from the same run stamp and
  build ID. Coverage manifests are read from `banks/play` and from the selected
  ROM's folder; performance bundles are read from `banks/play`.
- **Interpreter share** is `Tier-3 entries / (Tier-3 entries + dispatcher
  events)` for that run. Direct calls into recompiled C are not in the
  denominator, so the percentage describes dispatcher traffic, not wall time.
- **Interpreter coverage** shows the Tier-3 entry events, distinct fallback
  targets and interpreted instruction count in the cumulative coverage
  manifest.

The evidence row shows the build ID and timestamp. The panel warns when the
latest coverage belongs to a different ROM, predates the current bank set, or
has no performance log from the same run. With no matching evidence the value
is **not measured** (or **still hashing** while the selected ROM's SHA-1 is
computed). Enable **Keep coverage manifest** and **Diagnostics bundle** on Play,
run once, then return to Home or Build.

## Final review

Every action that starts a process — Configure, Build, tests, bootstrap, bank
generation, Play and Quickstart — first opens a scrollable **FINAL REVIEW**. It
lists the action, ROM and SHA-1, build and cache state, recompilation evidence,
bank settings, bootstrap options, play settings, every exact command with its
working directory, and a final check. Nothing runs until `Enter` or the `Run`
button is used. `Esc` or `Cancel` returns without starting anything.

The review is also the complete settings screen: advanced multi-title fields,
derived title config/overlay seed paths, resolved save path, extra arguments and
discovered tool/runner paths are all shown in one place.

## Mouse

Mouse support is built in:

- Click a tab to change pages.
- Click a selectable row to select and activate it: toggles flip, choices cycle,
  text and folder rows open their editor/browser, and actions open the review.
- Wheel over a page moves the selection; over the final review, browser or job
  log it scrolls that view.
- In the folder/ROM browser, the first click selects and clicking the selected
  row opens or picks it. The review's `Run` and `Cancel` buttons are clickable.
- Text selection still works in most terminals while holding `Shift`.

## Recommended workflow

1. **ROM**: choose the folder, then the ROM file.
2. **Build**: keep Build host tools on and run Configure & build to produce
   `pkmn_bankgen`.
3. **Setup**: Bootstrap recompiler once, then Generate title banks.
4. **Build**: enable Build runner (and Native present if wanted), then
   Configure & build again.
5. **Play**: choose presentation, save and diagnostics, then Play.

Faster path: choose the ROM, then press **Quickstart** on Home. It adds only the
missing setup steps and finishes with Play, after showing the full final review.

If banks or the runner are already built, skip the corresponding steps. The
Home page's next-step line reflects the current state.

## Local state and legal notes

- Settings live in `tui/.state.json`; the file is git-ignored. Override the
  path with `--state` or `PKMNG5PC_TUI_STATE`. The file contains paths and UI
  preferences only, never ROM data.
- The launcher reads ROM headers and hashes the selected ROM. It never copies,
  extracts or commits game content. Saves are written by the runner according
  to the Play page.
- `roms/`, `banks/` and `build/` stay local and git-ignored. Never commit ROMs,
  saves, extracted assets or generated bank sources.
- Bring your own legally obtained ROM. The launcher and the project ship no
  Nintendo/Game Freak assets.
