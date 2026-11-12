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
#   sh tools/play.sh --native                 # present through render/ (Vulkan)
#   sh tools/play.sh --native --gpu discrete  # pick the discrete Vulkan adapter
#   sh tools/play.sh --native --gpu index:1   # or pin a listed adapter
#   sh tools/play.sh --native --aspect 16:9   # widen the top screen band
#   sh tools/play.sh --native --window-size 3x --scaling fit
#   sh tools/play.sh --native --window 1280x960   # explicit pixel size
#   sh tools/play.sh --renderer3d compute --render-scale 4 --texture-upscale 4
#   sh tools/play.sh --renderer native        # 3D sink, slice 1 (4:3, 1x)
#   sh tools/play.sh --layout separate --renderer3d compute --render-scale 3
#                                             # hi-res 3D (OpenGL 4.3), 1440p-class
#   sh tools/play.sh --frames 600 --stats     # present 600 frames, print stats, exit
#   sh tools/play.sh --sav banks/black.sav    # keep a different save file
#   sh tools/play.sh --no-save                # blank 512 KiB cart, write nothing
#   sh tools/play.sh --banks banks/white      # check another title's bank dir
#   sh tools/play.sh --diagnostics            # keep a perf bundle in banks/play/
#
# --banks only moves the existence check -- the runner has its own bank
# directory compiled in at configure time (PKMNG5PC_BANKS_DIR /
# NDS_TITLE_BANK_DIRS). Pass the same directory here as the one the runner was
# built with so a missing or stale bank set is reported before boot.
#
# --native hands the two screens to this project's own present/compositor
# (`render/`, docs/adr/0006-ndsrecomp-native-present-hook.md) instead of the
# runner's fixed 512x768 SDL blit. It needs a runner built with
# PKMNG5PC_NATIVE_PRESENT=ON (the default). Stacked uses one window; separate
# opens one window per screen. --gpu picks the Vulkan adapter for --native:
# auto (prefer discrete), discrete, integrated, index:<n>, or a name substring.
# Everything else -- guest execution, input, audio, saves, the presented-frame
# digest -- is the same run either way.
#
# --renderer native enables the alpha Vulkan 3D sink and implies --native; the
# runner must be built with PKMNG5PC_NATIVE_RENDERER=ON. 3d, 2d and both are
# debug-only renderer selections, not normal play. Slice 1 renders 4:3 at 256 px
# and 1x; --aspect 16:9/21:9 or --render-scale above 1 therefore runs the CPU
# compositor instead of the sink and prints a note, rather than refusing to
# launch.
#
# --renderer3d compute replaces the 3D rasterizer with the vendored melonDS
# OpenGL 4.3 compute renderer: NDS_INTERNAL_RESOLUTION (1..4) then multiplies
# the 3D sample density, and above 1x the top screen is presented through the
# runner's own OpenGL window with the hi-res 3D composited into the 2D stack -
# so use --layout separate there and drop --native. Its render width must be a
# 64-multiple (256/320/384/448), so aspect 16:9 (342 px) stays on the faithful
# renderer. --renderer3d faithful (the default) pins the CPU rasterizer; without
# it an ambient NDS_3D_RENDERER decides.
#
# --window-size sizes each game window once at launch from the aspect and
# layout (2x of 16:9 stacked is 684x768); --window takes explicit pixels
# instead. The window stays freely resizable. --scaling integer (default) keeps
# whole-pixel blocks and letterboxes; fit fills the window at an
# aspect-preserving fractional scale. Both need --native.
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
banks_dir=banks/black
layout=stacked
native=0
gpu=${PKMNG5PC_GPU:-auto}
renderer=off
renderer3d=faithful
texture_upscale=
texture_upscale_set=0
aspect=4:3
render_scale=
aspect_set=0
render_scale_set=0
window_size=native
window=
window_set=0
scaling=integer
scaling_set=0
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
    --banks)    banks_dir=$2; shift 2 ;;
    --layout)   layout=$2; shift 2 ;;
    --native)   native=1; shift ;;
    --gpu)      gpu=$2; shift 2 ;;
    --renderer) renderer=$2; shift 2 ;;
    --renderer3d) renderer3d=$2; shift 2 ;;
    --texture-upscale) texture_upscale=$2; texture_upscale_set=1; shift 2 ;;
    --aspect)   aspect=$2; aspect_set=1; shift 2 ;;
    --render-scale) render_scale=$2; render_scale_set=1; shift 2 ;;
    --window-size) window_size=$2; shift 2 ;;
    --window)   window=$2; window_set=1; shift 2 ;;
    --scaling)  scaling=$2; scaling_set=1; shift 2 ;;
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

case $renderer in
  off)             renderer_on=0 ;;
  native|3d|2d|both) renderer_on=1 ;;
  *) fail "--renderer must be off, native, 3d, 2d or both." ;;
esac
case $aspect in
  4:3)  aspect_width=256 ;;
  5:3)  aspect_width=320 ;;
  16:9) aspect_width=342 ;;
  2:1)  aspect_width=384 ;;
  21:9) aspect_width=448 ;;
  *) fail "--aspect must be 4:3, 5:3, 16:9, 2:1 or 21:9." ;;
esac
if [ "$texture_upscale_set" -eq 1 ]; then
  case $texture_upscale in
    1|2|4) ;;
    *) fail "--texture-upscale must be 1, 2 or 4." ;;
  esac
fi
case $renderer3d in
  faithful) ;;
  compute)
    if [ "$renderer" != off ]; then
      fail "--renderer and --renderer3d compute are exclusive: the Vulkan 3D sink and the OpenGL compute renderer both replace the 3D rasterizer."
    fi
    if [ "$aspect" = 16:9 ]; then
      renderer3d=faithful
      echo "play: the compute renderer supports 256/320/384/448 px widths only; aspect 16:9 uses the faithful renderer" >&2
    fi
    ;;
  *) fail "--renderer3d must be faithful or compute." ;;
esac
if [ "$render_scale_set" -eq 1 ]; then
  case $render_scale in
    ''|*[!0-9]*) fail "--render-scale must be 1, 2, 3 or 4." ;;
  esac
  if [ "$render_scale" -lt 1 ] || [ "$render_scale" -gt 8 ]; then
    fail "--render-scale must be 1..8."
  fi
fi
window_text=$window
if [ -z "$window_text" ]; then
  case $window_size in
    native) ;;
    1x|2x|3x|4x)
      factor=${window_size%x}
      rows=384
      [ "$layout" = separate ] && rows=192
      window_text="$((aspect_width * factor))x$((rows * factor))" ;;
    *) fail "--window-size must be native, 1x, 2x, 3x or 4x." ;;
  esac
fi
if [ -n "$window_text" ]; then
  case $window_text in
    *x*) window_w=${window_text%x*}; window_h=${window_text#*x} ;;
    *) fail "--window must be WxH in pixels." ;;
  esac
  case $window_w in ''|*[!0-9]*) fail "--window must be WxH in pixels." ;; esac
  case $window_h in ''|*[!0-9]*) fail "--window must be WxH in pixels." ;; esac
  if [ "$window_w" -lt 160 ] || [ "$window_h" -lt 120 ] ||
     [ "$window_w" -gt 7680 ] || [ "$window_h" -gt 4320 ]; then
    fail "--window must be between 160x120 and 7680x4320."
  fi
fi
case $scaling in
  integer|fit) ;;
  *) fail "--scaling must be integer or fit." ;;
esac
if [ "$renderer_on" -eq 1 ]; then native=1; fi
if [ "$compute_hd" = true ] && [ "$native" -eq 1 ] && [ "$render_scale_set" -eq 1 ] &&
   [ "$render_scale" -gt 1 ]; then
  fail "--native and --renderer3d compute cannot both present: the hi-res image goes to the runner's own OpenGL window. Drop --native, or keep --native and 1x."
fi
if [ "$renderer_on" -eq 0 ] && [ "$render_scale_set" -eq 1 ]; then
  echo "play: --render-scale applies to --renderer or --renderer3d compute; ignored on the faithful soft renderer" >&2
fi
compute_hd=false
[ "$renderer3d" = compute ] && compute_hd=true
if [ "$renderer_on" -eq 0 ] && [ "$compute_hd" = false ] && [ "$aspect_set" -eq 1 ] && [ "$aspect" != "4:3" ]; then
  echo "play: --aspect applies to --native or --renderer3d compute; ignored on the SDL path" >&2
fi
if [ "$native" -eq 0 ] && { [ "$window_set" -eq 1 ] || [ "$window_size" != native ]; }; then
  echo "play: --window-size/--window apply to --native; ignored on the SDL path" >&2
fi
if [ "$native" -eq 0 ] && [ "$scaling_set" -eq 1 ]; then
  echo "play: --scaling applies to --native; ignored on the SDL path" >&2
fi

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

case $banks_dir in
  /*|[A-Za-z]:*) banks=$banks_dir ;;
  *)             banks="$root/$banks_dir" ;;
esac
set -- "$banks"/*_[0-9][0-9].c
[ -e "$1" ] || fail "no generated title banks in $banks. Generate them first:
  build/linux-debug/tools/pkmn_bankgen \"roms/<your-rom>.nds\" banks/black \\
      --recompiler third_party/ndsrecomp/recompiler/build/nds_recompile
or point --banks at the directory the runner was configured with
(PKMNG5PC_BANKS_DIR / NDS_TITLE_BANK_DIRS)."

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

game_code=$(game_code "$rom")
titles_file="$root/re/config/titles.tsv"
[ -f "$titles_file" ] || fail "missing $titles_file"
title_id=$(awk -F '\t' -v code="$game_code" '$1 == code { print $2; exit }' "$titles_file")
if [ "$renderer_on" -eq 1 ] && [ -z "$title_id" ]; then
  echo "play: no renderer profile for game code '$game_code'; PKMNG5PC_TITLE_ID stays unset" >&2
fi

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

# GPU choice belongs to the port's Vulkan presenter; the runner's SDL path has
# no adapter-selection hook. Say so rather than silently ignoring the flag.
if [ "$native" -eq 0 ] && [ "$gpu" != auto ]; then
  echo "play: --gpu applies to --native; the SDL path uses the system default adapter" >&2
fi

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
# The frontend hook reads this (render/src/native_present.cpp). Left alone
# otherwise so the environment stays the caller's.
if [ "$native" -eq 1 ]; then
  PKMNG5PC_PRESENT=native
  PKMNG5PC_GPU=$gpu
  PKMNG5PC_PRESENT_SCALING=$scaling
  export PKMNG5PC_PRESENT PKMNG5PC_GPU PKMNG5PC_PRESENT_SCALING
  if [ -n "$window_text" ]; then
    PKMNG5PC_WINDOW_SIZE=$window_text
    export PKMNG5PC_WINDOW_SIZE
  fi
fi
if [ "$native" -eq 1 ] || [ "$compute_hd" = true ]; then
  PKMNG5PC_WIDESCREEN_SUPPORTED=top
  export PKMNG5PC_WIDESCREEN_SUPPORTED
  if [ "$aspect" != "4:3" ]; then
    NDS_ADAPTIVE_WIDESCREEN=top
    NDS_WIDESCREEN_WIDTH=$aspect_width
    export NDS_ADAPTIVE_WIDESCREEN NDS_WIDESCREEN_WIDTH
  fi
fi
# Pin the 3D rasterizer: with the compute renderer built, the runner's auto
# policy would silently prefer it. The caller's own NDS_3D_RENDERER wins.
if [ "$compute_hd" = true ]; then
  NDS_3D_RENDERER=compute
  export NDS_3D_RENDERER
elif [ -z "${NDS_3D_RENDERER:-}" ]; then
  NDS_3D_RENDERER=soft
  export NDS_3D_RENDERER
fi
if [ "$renderer_on" -eq 1 ]; then
  PKMNG5PC_RENDERER=$renderer
  export PKMNG5PC_RENDERER
  if [ -n "$title_id" ]; then
    PKMNG5PC_TITLE_ID=$title_id
    export PKMNG5PC_TITLE_ID
  fi
fi
if [ "$render_scale_set" -eq 1 ]; then
  # The Vulkan sink reads this directly (1..8, render/src/ds). The compute
  # lane's own parser tops out at 4, so it only gets the in-range values.
  PKMNG5PC_RENDER_SCALE=$render_scale
  export PKMNG5PC_RENDER_SCALE
  if [ "$render_scale" -le 4 ]; then
    NDS_INTERNAL_RESOLUTION=$render_scale
    export NDS_INTERNAL_RESOLUTION
  fi
fi
if [ "$texture_upscale_set" -eq 1 ]; then
  # xBR texture upscaling: the vendored melonDS compute lane applies it to
  # every decoded 3D texture; the faithful path stays native. 1 = off.
  NDS_TEXTURE_UPSCALE=$texture_upscale
  export NDS_TEXTURE_UPSCALE
fi

echo "runner: $runner"
echo "rom:    $rom"
echo "banks:  $banks"
if [ "$native" -eq 1 ]; then
  echo "present: native (render/ - Vulkan, $layout, gpu=$gpu)"
elif [ "$compute_hd" = true ]; then
  scale_text=
  if [ "$render_scale_set" -eq 1 ] && [ "$render_scale" -gt 1 ]; then scale_text=", internal ${render_scale}x"; else scale_text=', internal 1x'; fi
  echo "present: runner OpenGL direct (compute renderer$scale_text, $layout)"
else
  echo "present: runner SDL blit ($layout)"
fi
if [ "$native" -eq 1 ]; then
  if [ -n "$window_text" ]; then window_label=$window_text; else window_label='runner default'; fi
  echo "output: aspect $aspect, window $window_label, $scaling scaling"
fi
if [ "$renderer_on" -eq 1 ]; then
  scale_text=
  [ "$render_scale_set" -eq 1 ] && scale_text=", scale ${render_scale}x"
  title_text=
  [ -n "$title_id" ] && title_text=", title $title_id"
  debug_text=
  case $renderer in 3d|2d|both) debug_text=" (debug)" ;; esac
  echo "render: $renderer$debug_text (aspect $aspect, source width $aspect_width$scale_text$title_text)"
elif [ "$compute_hd" = true ]; then
  echo "render: OpenGL 4.3 compute (aspect $aspect, source width $aspect_width)"
elif [ "$native" -eq 1 ]; then
  echo "render: native present, CPU compositor (aspect $aspect, source width $aspect_width)"
else
  echo "render: off (SDL blit; aspect and width unused)"
fi
if [ -n "$save_path" ]; then
  if [ -e "$save_path" ]; then echo "save:   $save_path"
  else echo "save:   $save_path (new: written when the game saves)"; fi
else
  echo "save:   off (--no-save, blank 512 KiB cart, nothing written)"
fi
echo "keys:   arrows=D-pad  Z=A X=B  A=Y S=X  Q=L W=R  Enter=Start  Backspace=Select  Esc=quit"

cd "$root"
exec "$@"
