# Run the game in the runner's live SDL window.
#
# Opens nds_runner's interactive frontend (`--interactive`) so the game is
# displayed as it executes, instead of the headless `--serve` path that
# tools/runwatch.py drives. See README.md ("Run") for the build steps this
# assumes: generated title banks plus a runner configured with
# PKMNG5PC_BUILD_RUNNER=ON (which supplies SDL3).
#
# Black is the default title, so the ROM defaults to the Black image in roms/.
# Other banked titles (White, Black 2, White 2) run with -Rom <path>; the runner
# registers their bank group by ROM SHA-1.
#
# Usage:
#   pwsh tools/play.ps1                       # play with the ROM's own save
#   pwsh tools/play.ps1 -Rom "roms/<your-rom>.nds"
#   pwsh tools/play.ps1 -Layout separate
#   pwsh tools/play.ps1 -Native               # present through render/ (Vulkan)
#   pwsh tools/play.ps1 -Native -Gpu discrete # pick the discrete Vulkan adapter
#   pwsh tools/play.ps1 -Native -Gpu "index:1" # or pin a listed adapter
#   pwsh tools/play.ps1 -Native -Aspect 16:9  # widen the top screen band
#   pwsh tools/play.ps1 -Native -WindowSize 3x -Scaling fit
#   pwsh tools/play.ps1 -Native -Window 1280x960   # explicit pixel size
#   pwsh tools/play.ps1 -Renderer native      # 3D sink, slice 1 (4:3, 1x)
#   pwsh tools/play.ps1 -Layout separate -Renderer3D compute -RenderScale 3
#                                             # hi-res 3D (OpenGL 4.3), 1440p-class
#   pwsh tools/play.ps1 -Layout separate -Renderer3D compute -RenderScale 4 -TextureUpscale 4
#                                             # xBR texture upscaling (smooth sprites)
#   pwsh tools/play.ps1 -Frames 600 -Stats    # present 600 frames, print stats, exit
#   pwsh tools/play.ps1 -Sav "banks/black.sav"  # keep a different save file
#   pwsh tools/play.ps1 -NoSave               # blank 512 KiB cart, write nothing
#   pwsh tools/play.ps1 -Banks banks/white    # check another title's bank dir
#   pwsh tools/play.ps1 -Diagnostics          # keep a perf bundle in banks/play/
#
# -Banks only moves the existence check -- the runner has its own bank
# directory compiled in at configure time (PKMNG5PC_BANKS_DIR /
# NDS_TITLE_BANK_DIRS). Pass the same directory here as the one the runner was
# built with so a missing or stale bank set is reported before boot.
#
# -Native hands the two screens to this project's own present/compositor
# (`render/`, docs/adr/0006-ndsrecomp-native-present-hook.md) instead of the
# runner's fixed 512x768 SDL blit. It needs a runner built with
# PKMNG5PC_NATIVE_PRESENT=ON (the default). Stacked uses one window; separate
# opens one window per screen. -Gpu picks the Vulkan adapter for -Native:
# auto (prefer discrete), discrete, integrated, index:<n>, or a name substring.
# Everything else -- guest execution, input, audio, saves, the presented-frame
# digest -- is the same run either way.
#
# -Renderer native enables the alpha Vulkan 3D sink and implies -Native; the
# runner must be built with PKMNG5PC_NATIVE_RENDERER=ON. 3d, 2d and both are
# debug-only renderer selections, not normal play. Slice 1 renders 4:3 at 256 px
# and 1x; -Aspect 16:9/21:9 or -RenderScale above 1 therefore runs the CPU
# compositor instead of the sink and prints a note, rather than refusing to
# launch.
#
# -Renderer3D compute replaces the 3D rasterizer with the vendored melonDS
# OpenGL 4.3 compute renderer: NDS_INTERNAL_RESOLUTION (1..4) then multiplies
# the 3D sample density, and above 1x the top screen is presented through the
# runner's own OpenGL window with the hi-res 3D composited into the 2D stack -
# so use -Layout separate there and drop -Native. Its render width must be a
# 64-multiple (256/320/384/448), so aspect 16:9 (342 px) stays on the faithful
# renderer. -Renderer3D faithful (the default) pins the CPU rasterizer; without
# it an ambient NDS_3D_RENDERER decides.
#
# -WindowSize sizes each game window once at launch from the aspect and layout
# (2x of 16:9 stacked is 684x768); -Window takes explicit pixels instead. The
# window stays freely resizable. -Scaling integer (default) keeps whole-pixel
# blocks and letterboxes; fit fills the window at an aspect-preserving
# fractional scale. Both need -Native.
#
# Saves are on by default: the run loads the save next to the ROM (<rom>.sav),
# starts blank when it is missing, and keeps writing it. -Sav (alias -SavePath)
# names another file and -NoSave keeps everything in RAM (the runner's
# --no-save). Either way the run passes re/config/frontend.toml, which declares
# the cartridge: a Gen 5 cart is 512 KiB flash, and the runner's 8 KiB EEPROM
# fallback makes the game's own save init fail.
#
# The positional BIOS directory and ARM9 cycle budget are still required by the
# runner, but the budget is inert under --interactive: the window stays open
# until you close it or press Esc.

param(
  [string]$Rom,
  [string]$Runner,
  [string]$BiosDir = 'bios',
  [string]$Banks = 'banks/black',
  [ValidateSet('stacked', 'separate')][string]$Layout = 'stacked',
  [switch]$Native,
  [string]$Gpu = '',
  [ValidateSet('off', 'native', '3d', '2d', 'both')][string]$Renderer = 'off',
  [ValidateSet('faithful', 'compute')][string]$Renderer3D = 'faithful',
  [ValidateSet(1, 2, 4)][int]$TextureUpscale = 1,
  [ValidateSet('4:3', '5:3', '16:9', '2:1', '21:9')][string]$Aspect = '4:3',
  [int]$RenderScale = 0,
  [ValidateSet('native', '1x', '2x', '3x', '4x')][string]$WindowSize = 'native',
  [ValidatePattern('^(\d+x\d+)?$')][string]$Window = '',
  [ValidateSet('integer', 'fit')][string]$Scaling = 'integer',
  [ValidateSet('nearest', 'linear')][string]$PresentFilter = 'nearest',
  [int]$Frames = 0,
  [Alias('SavePath')][string]$Sav,
  [switch]$Save,
  [switch]$NoSave,
  [switch]$Stats,
  [switch]$Coverage,
  [switch]$Diagnostics,
  [Parameter(ValueFromRemainingArguments = $true)][string[]]$Extra
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot

if (-not $Gpu) {
  $Gpu = if ($env:PKMNG5PC_GPU) { $env:PKMNG5PC_GPU } else { 'auto' }
}

function Fail([string]$message) {
  Write-Host "play: $message" -ForegroundColor Red
  exit 1
}

$rendererOn = $Renderer -ne 'off'
$nativePresent = $Native -or $rendererOn
$computeHd = $Renderer3D -eq 'compute'
if ($computeHd -and $rendererOn) {
  Fail '-Renderer and -Renderer3D compute are exclusive: the Vulkan 3D sink and the OpenGL compute renderer both replace the 3D rasterizer.'
}
$renderScaleSet = $PSBoundParameters.ContainsKey('RenderScale')
if ($renderScaleSet -and ($RenderScale -lt 1 -or $RenderScale -gt 8)) {
  Fail '-RenderScale must be 1..8.'
}
if ($computeHd -and $renderScaleSet -and $RenderScale -gt 1 -and $Native) {
  Fail '-Native and -Renderer3D compute cannot both present: the hi-res image is presented through the runner''s own OpenGL window. Drop -Native, or keep -Native and 1x.'
}
# The compute rasterizer bakes ScreenWidth into its shaders, so its render
# width must stay a 64-multiple: 256, 320, 384 or 448. 342 (16:9) stays a
# faithful-path width; fall back rather than dying at compute init.
if ($computeHd -and $Aspect -eq '16:9') {
  $Renderer3D = 'faithful'
  $computeHd = $false
  Write-Host 'play: the compute renderer supports 256/320/384/448 px widths only; aspect 16:9 uses the faithful renderer' -ForegroundColor Yellow
}
$aspectWidths = @{
  '4:3' = 256; '5:3' = 320; '16:9' = 342; '2:1' = 384; '21:9' = 448
}
$windowText = $Window
if (-not $windowText -and $WindowSize -ne 'native') {
  $factor = [int]$WindowSize.Substring(0, 1)
  $rows = if ($Layout -eq 'separate') { 192 } else { 384 }
  $windowText = '{0}x{1}' -f ($aspectWidths[$Aspect] * $factor), ($rows * $factor)
}
if ($windowText) {
  $parts = $windowText -split 'x'
  $windowWidth = [int]$parts[0]
  $windowHeight = [int]$parts[1]
  if ($windowWidth -lt 160 -or $windowHeight -lt 120 -or $windowWidth -gt 7680 -or
      $windowHeight -gt 4320) {
    Fail '-Window must be between 160x120 and 7680x4320.'
  }
}
$titlesFile = Join-Path $root 're/config/titles.tsv'
if (-not (Test-Path -LiteralPath $titlesFile)) { Fail "missing $titlesFile" }
$titleIds = @{}
Get-Content -LiteralPath $titlesFile |
  Where-Object { $_ -and -not $_.StartsWith('#') } |
  ForEach-Object {
    $fields = $_ -split "`t"
    if ($fields.Count -ge 2 -and $fields[0] -ne 'code') { $titleIds[$fields[0]] = $fields[1] }
  }
if (-not $nativePresent -and -not $computeHd -and $PSBoundParameters.ContainsKey('Aspect') -and $Aspect -ne '4:3') {
  Write-Host "play: -Aspect applies to -Native or -Renderer3D compute; ignored on the SDL path" -ForegroundColor Yellow
}
if (-not $rendererOn -and -not $computeHd -and $renderScaleSet) {
  Write-Host "play: -RenderScale applies to -Renderer or -Renderer3D compute; ignored on the faithful soft renderer" -ForegroundColor Yellow
}
if (-not $nativePresent -and ($WindowSize -ne 'native' -or $Window)) {
  Write-Host "play: -WindowSize/-Window apply to -Native; ignored on the SDL path" -ForegroundColor Yellow
}
if (-not $nativePresent -and $PSBoundParameters.ContainsKey('Scaling')) {
  Write-Host "play: -Scaling applies to -Native; ignored on the SDL path" -ForegroundColor Yellow
}

# --- runner -----------------------------------------------------------------

$candidates = @()
if ($Runner) { $candidates += $Runner }
if ($env:PKMNG5PC_RUNNER) { $candidates += $env:PKMNG5PC_RUNNER }
foreach ($preset in 'windows-relwithdebinfo', 'windows-release', 'windows-debug') {
  $candidates += Join-Path $root "build/$preset/third_party/ndsrecomp/runner/nds_runner.exe"
}
$runner = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $runner) {
  Fail @"
no built runner found. Configure and build one first:
  cmake --preset windows-relwithdebinfo -DPKMNG5PC_BUILD_RUNNER=ON
  cmake --build --preset windows-relwithdebinfo
or pass -Runner <path>.
"@
}

# --- banks ------------------------------------------------------------------

$banks = if ([System.IO.Path]::IsPathRooted($Banks)) { $Banks } else { Join-Path $root $Banks }
$bodies = @(Get-ChildItem $banks -File -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -match '_[0-9][0-9]\.c$' })
if ($bodies.Count -eq 0) {
  Fail @"
no generated title banks in $banks. Generate them first:
  build/windows-debug/tools/pkmn_bankgen.exe "roms/<your-rom>.nds" banks/black ``
      --recompiler third_party/ndsrecomp/recompiler/build/nds_recompile.exe
or point -Banks at the directory the runner was configured with
(PKMNG5PC_BANKS_DIR / NDS_TITLE_BANK_DIRS).
"@
}

# --- ROM --------------------------------------------------------------------

function Get-GameCode([string]$path) {
  $stream = [System.IO.File]::OpenRead($path)
  try {
    $buffer = New-Object byte[] 16
    [void]$stream.Read($buffer, 0, 16)
  } finally {
    $stream.Dispose()
  }
  return [System.Text.Encoding]::ASCII.GetString($buffer, 12, 4)
}

if (-not $Rom) { $Rom = $env:PKMNG5PC_ROM }
if (-not $Rom) {
  $candidates = @(Get-ChildItem (Join-Path $root 'roms') -Filter '*.nds' -ErrorAction SilentlyContinue)
  $black = $candidates | Where-Object { (Get-GameCode $_.FullName) -eq 'IRBO' } | Select-Object -First 1
  if ($black) {
    $Rom = $black.FullName
  } elseif ($candidates.Count -eq 1) {
    $Rom = $candidates[0].FullName
  } else {
    $names = ($candidates | ForEach-Object { "  $($_.Name)" }) -join "`n"
    Fail "no Black ROM (game code IRBO) found in roms/ - pass -Rom <path> to run another banked title (White, Black 2, White 2).`nCandidates:`n$names"
  }
}
if (-not (Test-Path -LiteralPath $Rom)) { Fail "no such ROM: $Rom" }
$Rom = (Resolve-Path -LiteralPath $Rom).Path
$gameCode = Get-GameCode $Rom
$titleId = $titleIds[$gameCode]
if ($rendererOn -and -not $titleId) {
  Write-Host "play: no renderer profile for game code '$gameCode'; PKMNG5PC_TITLE_ID stays unset" -ForegroundColor Yellow
}

# --- command ----------------------------------------------------------------

if ($NoSave -and ($PSBoundParameters.ContainsKey('Sav') -or $Save)) {
  Fail '-NoSave cannot be combined with -Sav/-Save.'
}
# GPU choice belongs to the port's Vulkan presenter; the runner's SDL path has
# no adapter-selection hook. Say so rather than silently ignoring the flag.
if (-not $nativePresent -and $Gpu -ne 'auto') {
  Write-Host "play: -Gpu applies to -Native; the SDL path uses the system default adapter" -ForegroundColor Yellow
}
# The default is the derivation the runner itself would make: the ROM path with
# a .sav extension, i.e. the save sits next to the ROM. An explicit -Sav/-Save
# with an empty value names that same file.
$savePath = if ($NoSave) { '' } else { [System.IO.Path]::ChangeExtension($Rom, '.sav') }
if (-not $NoSave -and $Sav) { $savePath = $Sav }

$runnerArgs = @(
  $BiosDir, '250000000',
  '--boot', 'direct', '--generated-firmware', '--freebios',
  '--rom', $Rom,
  '--interactive',
  '--screen-layout', $Layout
)
if (-not $Coverage) { $runnerArgs += '--no-coverage-manifest' }
if ($Diagnostics) {
  $runnerArgs += @('--diagnostics', 'on', '--diagnostics-dir', (Join-Path $root 'banks/play'))
} else {
  $runnerArgs += @('--diagnostics', 'off')
}
# The config describes the cartridge, not persistence: the runner's historical
# default (8 KiB EEPROM) makes the game's save init fail, so it is always passed.
$frontendConfig = Join-Path $root 're/config/frontend.toml'
if (-not (Test-Path -LiteralPath $frontendConfig)) { Fail "missing $frontendConfig" }
$runnerArgs += @('--config', $frontendConfig)
if ($savePath) { $runnerArgs += @('--save-path', $savePath) } else { $runnerArgs += '--no-save' }
if ($Extra) { $runnerArgs += $Extra }

if ($Frames -gt 0) { $env:NDS_FRONTEND_MAX_FRAMES = "$Frames" }
if ($Stats -or $Frames -gt 0) { $env:NDS_FRONTEND_STATS = '1' }
# The runner's threaded 2D scanline path wedges a single scheduler round at
# t=83.69 s emulated (re/notes/recompilation.md). The inline path is the same
# latch/execute sequence and is byte-locked to the threaded one by the
# presented-frame digest ring, so default to it unless the caller overrides.
if (-not $env:NDS_GPU2D_THREADED) { $env:NDS_GPU2D_THREADED = '0' }
# The frontend hook reads this (render/src/native_present.cpp). Leave it alone
# otherwise so the environment stays the caller's.
if ($nativePresent) {
  $env:PKMNG5PC_PRESENT = 'native'
  $env:PKMNG5PC_GPU = $Gpu
  $env:PKMNG5PC_PRESENT_SCALING = $Scaling
  $env:PKMNG5PC_PRESENT_FILTER = $PresentFilter
  if ($windowText) { $env:PKMNG5PC_WINDOW_SIZE = $windowText }
}
# The wide top band works on the native-present path and on the compute
# renderer; the plain SDL path has no wide consumer.
if ($nativePresent -or $computeHd) {
  $env:PKMNG5PC_WIDESCREEN_SUPPORTED = 'top'
  if ($Aspect -ne '4:3') {
    $env:NDS_ADAPTIVE_WIDESCREEN = 'top'
    $env:NDS_WIDESCREEN_WIDTH = "$($aspectWidths[$Aspect])"
  }
}
# Pin the 3D rasterizer: with the compute renderer built, the runner's auto
# policy would silently prefer it. The caller's own NDS_3D_RENDERER wins.
if ($computeHd) {
  $env:NDS_3D_RENDERER = 'compute'
} elseif (-not $env:NDS_3D_RENDERER) {
  $env:NDS_3D_RENDERER = 'soft'
}
if ($rendererOn) {
  $env:PKMNG5PC_RENDERER = $Renderer
  if ($titleId) { $env:PKMNG5PC_TITLE_ID = $titleId }
}
if ($renderScaleSet) {
  # The Vulkan sink reads this directly (1..8, render/src/ds). The compute
  # lane's own parser tops out at 4, so it only gets the in-range values.
  $env:PKMNG5PC_RENDER_SCALE = "$RenderScale"
  if ($RenderScale -le 4) { $env:NDS_INTERNAL_RESOLUTION = "$RenderScale" }
}
if ($PSBoundParameters.ContainsKey('TextureUpscale')) {
  # xBR texture upscaling (the vendored melonDS compute lane applies it to
  # every decoded 3D texture; the faithful path stays native): 1 = off.
  $env:NDS_TEXTURE_UPSCALE = "$TextureUpscale"
}

Write-Host "runner: $runner"
Write-Host "rom:    $Rom"
Write-Host "banks:  $banks"
if ($computeHd) {
  $scaleText = if ($renderScaleSet -and $RenderScale -gt 1) { ", internal $($RenderScale)x" } else { ', internal 1x' }
  Write-Host "present: runner OpenGL direct (compute renderer$scaleText, $Layout)"
} elseif ($nativePresent) {
  Write-Host "present: native (render/ - Vulkan, $Layout, gpu=$Gpu)"
} else {
  Write-Host "present: runner SDL blit ($Layout)"
}
if ($nativePresent) {
  $windowLabel = if ($windowText) { $windowText } else { 'runner default' }
  Write-Host "output: aspect $Aspect, window $windowLabel, $Scaling scaling"
}
if ($rendererOn) {
  $scaleText = if ($renderScaleSet) { ", scale $($RenderScale)x" } else { '' }
  $titleText = if ($titleId) { ", title $titleId" } else { '' }
  $debugText = if ($Renderer -in @('3d', '2d', 'both')) { ' (debug)' } else { '' }
  Write-Host "render: $Renderer$debugText (aspect $Aspect, source width $($aspectWidths[$Aspect])$scaleText$titleText)"
} elseif ($computeHd) {
  Write-Host "render: OpenGL 4.3 compute (aspect $Aspect, source width $($aspectWidths[$Aspect]))"
} elseif ($nativePresent) {
  Write-Host "render: native present, CPU compositor (aspect $Aspect, source width $($aspectWidths[$Aspect]))"
} else {
  Write-Host "render: off (SDL blit; aspect and width unused)"
}
if ($savePath) {
  $fresh = if (Test-Path -LiteralPath $savePath) { '' } else { ' (new: written when the game saves)' }
  Write-Host "save:   $savePath$fresh"
} else {
  Write-Host "save:   off (--no-save, blank 512 KiB cart, nothing written)"
}
Write-Host "keys:   arrows=D-pad  Z=A X=B  A=Y S=X  Q=L W=R  Enter=Start  Backspace=Select  Esc=quit"

Push-Location $root
try {
  & $runner @runnerArgs
  exit $LASTEXITCODE
} finally {
  Pop-Location
}
