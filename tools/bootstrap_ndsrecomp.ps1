# Bootstrap + build the ndsrecomp recompiler + runner (pinned git submodule).
#
# Reproduces the steps needed on a fresh clone:
#   1. init the ndsrecomp submodule (and its broken-URL nested submodule),
#   2. stage the toml++ single header (raw.githubusercontent is blocked),
#   3. build the recompiler,
#   4. generate the FreeBIOS banks + placeholder retail BIOS banks,
#   5. optionally configure + build a headless (or SDL3) runner.
#
# Usage:
#   pwsh tools/bootstrap_ndsrecomp.ps1
#   pwsh tools/bootstrap_ndsrecomp.ps1 -BuildRunner
#   pwsh tools/bootstrap_ndsrecomp.ps1 -BuildRunner -Sdl3

param(
  [switch]$BuildRunner,
  [switch]$Sdl3,
  [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$eng  = Join-Path $root 'third_party/ndsrecomp'
$gen  = Join-Path $eng  'generated'

function Step([string]$msg) { Write-Host "==> $msg" -ForegroundColor Cyan }
function Invoke-Native { & $args[0] @($args[1..($args.Count - 1)]); if ($LASTEXITCODE -ne 0) { throw "command failed: $($args -join ' ')" } }

Step 'Init ndsrecomp submodule'
Invoke-Native git -C $root submodule update --init -- third_party/ndsrecomp

# The v0.6.10 nested submodule URL is relative and broken; point it at GitHub.
$nested = Join-Path $eng 'external/arm-recomp-core'
if (-not (Test-Path (Join-Path $nested 'CMakeLists.txt'))) {
  Invoke-Native git -C $eng config submodule.external/arm-recomp-core.url https://github.com/mstan/arm-recomp-core.git
}
Invoke-Native git -C $eng submodule update --init --recursive

Step 'Stage toml++ single header'
$toml = Join-Path $eng 'recompiler/third_party/toml.hpp'
if (-not (Test-Path $toml)) {
  New-Item -ItemType Directory -Force -Path (Split-Path $toml) | Out-Null
  $tmp = Join-Path $env:TEMP ("tomlplusplus-" + [guid]::NewGuid().ToString('N'))
  Invoke-Native git clone --depth 1 --branch v3.4.0 https://github.com/marzer/tomlplusplus.git $tmp
  Copy-Item (Join-Path $tmp 'toml.hpp') $toml -Force
  Remove-Item -Recurse -Force $tmp
}

Step 'Build recompiler'
Invoke-Native cmake -G Ninja -S (Join-Path $eng 'recompiler') -B (Join-Path $eng 'recompiler/build') "-DCMAKE_BUILD_TYPE=$Configuration"
Invoke-Native cmake --build (Join-Path $eng 'recompiler/build')

Step 'Generate BIOS banks'
$rc  = Join-Path $eng 'recompiler/build/nds_recompile.exe'
$f9  = Join-Path $eng 'third_party/freebios/drastic_bios_arm9.bin'
$f7  = Join-Path $eng 'third_party/freebios/drastic_bios_arm7.bin'
$c9  = Join-Path $eng 'bios/freebios9.toml'
$c7  = Join-Path $eng 'bios/freebios7.toml'
Invoke-Native $rc --config $c9 --bin $f9 --out $gen --bank freebios_arm9
Invoke-Native $rc --config $c7 --bin $f7 --out $gen --bank freebios_arm7
# Retail BIOS banks must exist to link, but no retail dump is shipped; emit
# FreeBIOS-content placeholders. The runtime uses the FreeBIOS banks by default.
Invoke-Native $rc --config $c9 --bin $f9 --out $gen --bank arm9_bios
Invoke-Native $rc --config $c7 --bin $f7 --out $gen --bank arm7_bios

if ($BuildRunner) {
  Step 'Configure + build runner'
  $backend = if ($Sdl3) { 'SDL3' } else { 'NONE' }
  $compute = if ($Sdl3) { 'ON' } else { 'OFF' }
  Invoke-Native cmake -G Ninja -S (Join-Path $eng 'runner') -B (Join-Path $eng 'runner/build') `
    "-DCMAKE_BUILD_TYPE=$Configuration" `
    "-DNDS_SDL_BACKEND=$backend" `
    "-DNDS_ENABLE_COMPUTE_RENDERER=$compute" `
    -DNDS_BOOTSTRAP_FIRMWARE=ON
  Invoke-Native cmake --build (Join-Path $eng 'runner/build')
}

Step 'Done'
