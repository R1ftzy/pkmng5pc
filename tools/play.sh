#!/bin/sh
# Run the game in the runner's live SDL window.
#
# Opens nds_runner's interactive frontend (`--interactive`) so the game is
# displayed as it executes, instead of the headless `--serve` path that
# tools/runwatch.py drives. See README.md ("Run") for the build steps this
# assumes: generated title banks plus a runner configured with
# PKMNG5PC_BUILD_RUNNER=ON (which supplies SDL3).
#
# Black is the default title, so the ROM defaults to the Black image in roms/.
# Other banked titles (White, Black 2, White 2) run with --rom <path>; the
# runner registers their bank group by ROM SHA-1.
#
# Usage:
#   sh tools/play.sh                          # play with the ROM's own save
#   sh tools/play.sh --rom "roms/<your-rom>.nds"
#   sh tools/play.sh --layout separate
#   sh tools/play.sh --frames 600 --stats     # present 600 frames, print stats, exit
#   sh tools/play.sh --sav banks/black.sav    # keep a different save file
#   sh tools/play.sh --no-save                # blank 512 KiB cart, write nothing
#   sh tools/play.sh --diagnostics            # keep a perf bundle in banks/play/
#
# Saves are on by default: the run loads the save next to the ROM (<rom>.sav),
# starts blank when it is missing, and keeps writing it. --sav (alias
# --save-path) names another file and --no-save keeps everything in RAM. Either
# way the run passes re/config/frontend.toml, which declares the cartridge: a
# Gen 5 cart is 512 KiB flash, and the runner's 8 KiB EEPROM fallback makes the
# game's own save init fail.
#
# The positional BIOS directory and ARM9 cycle budget are still required by the
# runner, but the budget is inert under --interactive: the window stays open
# until you close it or press Esc.

set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

rom=${PKMNG5PC_ROM:-}
runner=${PKMNG5PC_RUNNER:-}
bios_dir=bios
layout=stacked
frames=0
stats=0
coverage=0
save_path=
save_path_set=0
no_save=0
diagnostics=0
extra=
while [ $# -gt 0 ]; do
  case $1 in
    --rom)      rom=$2; shift 2 ;;
    --runner)   runner=$2; shift 2 ;;
    --bios-dir) bios_dir=$2; shift 2 ;;
    --layout)   layout=$2; shift 2 ;;
    --frames)   frames=$2; shift 2 ;;
    --save-path) save_path=$2; save_path_set=1; shift 2 ;;
    --sav)      save_path=$2; save_path_set=1; shift 2 ;;
    --save)     save_path=; save_path_set=1; shift ;;
    --no-save)  no_save=1; shift ;;
    --stats)    stats=1; shift ;;
    --coverage) coverage=1; shift ;;
    --diagnostics) diagnostics=1; shift ;;
    --)         shift; extra=$*; break ;;
    -h|--help)  sed -n '2,/^[^#]/p' "$0" | sed -e '$d' -e 's/^# \{0,1\}//'; exit 0 ;;
    *)          echo "play: unknown option: $1" >&2; exit 2 ;;
  esac
done

fail() { echo "play: $1" >&2; exit 1; }

# --- runner -----------------------------------------------------------------

if [ -z "$runner" ]; then
  for preset in linux-relwithdebinfo linux-release linux-debug; do
    candidate="$root/build/$preset/third_party/ndsrecomp/runner/nds_runner"
    if [ -x "$candidate" ]; then runner=$candidate; break; fi
  done
fi
[ -n "$runner" ] || fail "no built runner found. Configure and build one first:
  cmake --preset linux-relwithdebinfo -DPKMNG5PC_BUILD_RUNNER=ON
  cmake --build --preset linux-relwithdebinfo
or pass --runner <path>."

# --- banks ------------------------------------------------------------------

banks="$root/banks/black"
set -- "$banks"/*_[0-9][0-9].c
[ -e "$1" ] || fail "no generated title banks in $banks. Generate them first:
  build/linux-debug/tools/pkmn_bankgen \"roms/<your-rom>.nds\" banks/black \\
      --recompiler third_party/ndsrecomp/recompiler/build/nds_recompile"

# --- ROM --------------------------------------------------------------------

# The NDS header carries the 4-byte game code at 0x0C; Black is IRBO.
game_code() { dd if="$1" bs=1 skip=12 count=4 2>/dev/null; }

if [ -z "$rom" ]; then
  black=
  count=0
  for f in "$root"/roms/*.nds; do
    [ -e "$f" ] || continue
    count=$((count + 1))
    if [ "$(game_code "$f")" = "IRBO" ] && [ -z "$black" ]; then black=$f; fi
  done
  if [ -n "$black" ]; then
    rom=$black
  elif [ "$count" -eq 1 ]; then
    for f in "$root"/roms/*.nds; do rom=$f; done
  else
    fail "no Black ROM (game code IRBO) found in roms/ - pass --rom <path> to run another banked title (White, Black 2, White 2)."
  fi
fi
[ -f "$rom" ] || fail "no such ROM: $rom"

# --- save -------------------------------------------------------------------

if [ "$no_save" -eq 1 ] && [ "$save_path_set" -eq 1 ]; then
  fail "--no-save cannot be combined with --sav/--save-path."
fi
# The default is the same derivation the runner itself would make: <rom>.sav
# beside the ROM. An explicit --sav/--save with an empty value names that file.
if [ "$no_save" -eq 0 ] && [ -z "$save_path" ]; then
  save_path=${rom%.*}.sav
fi

# --- command ----------------------------------------------------------------

set -- "$runner" "$bios_dir" 250000000 \
  --boot direct --generated-firmware --freebios \
  --rom "$rom" --interactive --screen-layout "$layout"
[ "$coverage" -eq 1 ] && set -- "$@" || set -- "$@" --no-coverage-manifest
if [ "$diagnostics" -eq 1 ]; then set -- "$@" --diagnostics on --diagnostics-dir "$root/banks/play"
else set -- "$@" --diagnostics off; fi
# The config describes the cartridge, not persistence: the runner's historical
# default (8 KiB EEPROM) makes the game's save init fail, so it is always passed.
frontend_config="$root/re/config/frontend.toml"
[ -f "$frontend_config" ] || fail "missing $frontend_config"
set -- "$@" --config "$frontend_config"
if [ -n "$save_path" ]; then set -- "$@" --save-path "$save_path"
else set -- "$@" --no-save; fi
[ -n "$extra" ] && set -- "$@" $extra

[ "$frames" -gt 0 ] && NDS_FRONTEND_MAX_FRAMES=$frames && export NDS_FRONTEND_MAX_FRAMES
if [ "$stats" -eq 1 ] || [ "$frames" -gt 0 ]; then NDS_FRONTEND_STATS=1; export NDS_FRONTEND_STATS; fi
# Same reason as tools/play.ps1: the threaded 2D scanline path wedges one
# scheduler round at t=83.69 s emulated; run the byte-locked inline path.
if [ -z "${NDS_GPU2D_THREADED:-}" ]; then NDS_GPU2D_THREADED=0; export NDS_GPU2D_THREADED; fi

echo "runner: $runner"
echo "rom:    $rom"
echo "banks:  $banks"
if [ -n "$save_path" ]; then
  if [ -e "$save_path" ]; then echo "save:   $save_path"
  else echo "save:   $save_path (new: written when the game saves)"; fi
else
  echo "save:   off (--no-save, blank 512 KiB cart, nothing written)"
fi
echo "keys:   arrows=D-pad  Z=A X=B  A=Y S=X  Q=L W=R  Enter=Start  Backspace=Select  Esc=quit"

cd "$root"
exec "$@"
