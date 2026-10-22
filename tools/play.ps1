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
#   pwsh tools/play.ps1 -Renderer native      # 3D sink, slice 1 (4:3, 1x)
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
# -Renderer native enables the alpha 3D sink and implies -Native; the runner
# must be built with PKMNG5PC_NATIVE_RENDERER=ON. 3d, 2d and both are debug-only
# renderer selections, not normal play. Slice 1 renders 4:3 at 256 px and 1x:
# the bridge rejects other aspect widths, and the sink does not use
# -RenderScale yet.
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
  [ValidateSet('4:3', '16:9', '21:9')][string]$Aspect = '4:3',
  [int]$RenderScale = 0,
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
$renderScaleSet = $PSBoundParameters.ContainsKey('RenderScale')
if ($renderScaleSet -and ($RenderScale -lt 1 -or $RenderScale -gt 4)) {
  Fail '-RenderScale must be 1, 2, 3 or 4.'
}
$aspectWidths = @{ '4:3' = 256; '16:9' = 342; '21:9' = 448 }
$titlesFile = Join-Path $root 're/config/titles.tsv'
if (-not (Test-Path -LiteralPath $titlesFile)) { Fail "missing $titlesFile" }
$titleIds = @{}
Get-Content -LiteralPath $titlesFile |
  Where-Object { $_ -and -not $_.StartsWith('#') } |
  ForEach-Object {
    $fields = $_ -split "`t"
    if ($fields.Count -ge 2 -and $fields[0] -ne 'code') { $titleIds[$fields[0]] = $fields[1] }
  }
if (-not $rendererOn -and $PSBoundParameters.ContainsKey('Aspect') -and $Aspect -ne '4:3') {
  Write-Host "play: -Aspect applies to -Renderer; ignored while the renderer is off" -ForegroundColor Yellow
}
if (-not $rendererOn -and $renderScaleSet) {
  Write-Host "play: -RenderScale applies to -Renderer; ignored while the renderer is off" -ForegroundColor Yellow
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
}
if ($rendererOn) {
  $env:PKMNG5PC_RENDERER = $Renderer
  $env:PKMNG5PC_WIDESCREEN_SUPPORTED = 'top'
  # Slice 1 renders 4:3 at 256: request no adaptive widening at native width.
  if ($Aspect -ne '4:3') {
    $env:NDS_ADAPTIVE_WIDESCREEN = 'top'
    $env:NDS_WIDESCREEN_WIDTH = "$($aspectWidths[$Aspect])"
  }
  if ($titleId) { $env:PKMNG5PC_TITLE_ID = $titleId }
  if ($renderScaleSet) { $env:NDS_INTERNAL_RESOLUTION = "$RenderScale" }
}

Write-Host "runner: $runner"
Write-Host "rom:    $Rom"
Write-Host "banks:  $banks"
if ($nativePresent) {
  Write-Host "present: native (render/ - Vulkan, $Layout, gpu=$Gpu)"
} else {
  Write-Host "present: runner SDL blit ($Layout)"
}
if ($rendererOn) {
  $scaleText = if ($renderScaleSet) { ", scale $($RenderScale)x" } else { '' }
  $titleText = if ($titleId) { ", title $titleId" } else { '' }
  $debugText = if ($Renderer -in @('3d', '2d', 'both')) { ' (debug)' } else { '' }
  Write-Host "render: $Renderer$debugText (aspect $Aspect, source width $($aspectWidths[$Aspect])$scaleText$titleText)"
} else {
  Write-Host "render: off (host CPU; aspect and width unused)"
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
