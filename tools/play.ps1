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
# PKMNG5PC_NATIVE_PRESENT=ON (the default) and is single-window, so it always
# presents stacked. Everything else -- guest execution, input, audio, saves,
# the presented-frame digest -- is the same run either way.
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

function Fail([string]$message) {
  Write-Host "play: $message" -ForegroundColor Red
  exit 1
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

# --- command ----------------------------------------------------------------

if ($NoSave -and ($PSBoundParameters.ContainsKey('Sav') -or $Save)) {
  Fail '-NoSave cannot be combined with -Sav/-Save.'
}
# The port's presenter owns one output surface, so -Native is always stacked.
if ($Native -and $Layout -ne 'stacked') {
  Write-Host "play: -Native presents one window; ignoring -Layout $Layout" -ForegroundColor Yellow
  $Layout = 'stacked'
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
if ($Native) { $env:PKMNG5PC_PRESENT = 'native' }

Write-Host "runner: $runner"
Write-Host "rom:    $Rom"
Write-Host "banks:  $banks"
if ($Native) {
  Write-Host "present: native (render/ - Vulkan, stacked, single window)"
} else {
  Write-Host "present: runner SDL blit (512x768 top-over-bottom)"
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
