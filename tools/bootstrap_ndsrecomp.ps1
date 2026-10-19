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
#   pwsh tools/bootstrap_ndsrecomp.ps1 -Compiler clang
#   pwsh tools/bootstrap_ndsrecomp.ps1 -Compiler C:/tools/llvm-mingw/bin/clang.exe
#
# -Compiler picks the toolchain for the recompiler (and, with -BuildRunner,
# the runner): a bare name resolved from PATH (clang, clang-18, gcc) or a path
# to the C compiler. The C++ sibling is derived (clang -> clang++, gcc -> g++)
# and, for a path, must sit next to it. Switching compilers on an existing
# build directory is refused -- remove third_party/ndsrecomp/recompiler/build
# (and runner/build) first; CMake caches the compiler choice.

param(
  [switch]$BuildRunner,
  [switch]$Sdl3,
  [string]$Configuration = 'Release',
  [string]$Compiler
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$eng  = Join-Path $root 'third_party/ndsrecomp'
$gen  = Join-Path $eng  'generated'

function Step([string]$msg) { Write-Host "==> $msg" -ForegroundColor Cyan }
function Invoke-Native {
  # Flatten array arguments (e.g. a list of -D flags) into separate argv
  # entries instead of letting PowerShell stringify them into one.
  $flat = @()
  foreach ($a in $args) { $flat += $a }
  & $flat[0] @($flat[1..($flat.Count - 1)])
  if ($LASTEXITCODE -ne 0) { throw "command failed: $($flat -join ' ')" }
}

# The C++ compiler is the C compiler's sibling (clang -> clang++, gcc -> g++);
# 'cl' serves both. Anything else is refused rather than guessed.
function Resolve-Toolchain([string]$cc) {
  $dir = $null
  if ($cc -match '[\\/]') {
    if (-not (Test-Path -LiteralPath $cc)) { throw "no such compiler: $cc" }
    $cc = (Resolve-Path -LiteralPath $cc).Path
    $dir = Split-Path -Parent $cc
  }
  $leaf = Split-Path -Leaf $cc
  $ext = [System.IO.Path]::GetExtension($leaf)
  $stem = [System.IO.Path]::GetFileNameWithoutExtension($leaf)
  $cxxStem = switch -Regex ($stem) {
    '^clang' { 'clang++' + $stem.Substring(5) }
    '^gcc' { 'g++' + $stem.Substring(3) }
    '^cl$' { 'cl' }
    default { throw "don't know the C++ sibling of '$leaf'; pass a clang/gcc-family compiler" }
  }
  $cxx = $cxxStem + $ext
  if ($dir) {
    $cxx = Join-Path $dir $cxx
    if (-not (Test-Path -LiteralPath $cxx)) { throw "C++ compiler not found next to it: $cxx" }
  }
  return @($cc, $cxx)
}

# CMake caches the compiler, and re-pointing it makes a stale build dir fail in
# opaque ways. Refuse the mismatch instead of letting CMake guess. Only the
# executable's name matters (a bare 'clang++' and a full path to clang++.exe are
# the same compiler; 'clang-18' is not).
function Compiler-Key([string]$path) {
  return [System.IO.Path]::GetFileNameWithoutExtension($path).ToLowerInvariant()
}

function Assert-Compiler-Cache([string]$buildDir, [string]$cxx) {
  $cache = Join-Path $buildDir 'CMakeCache.txt'
  if (-not (Test-Path $cache)) { return }
  $line = Select-String -Path $cache -Pattern '^CMAKE_CXX_COMPILER:[^=]+=(.*)$' | Select-Object -First 1
  if (-not $line) { return }
  $recorded = $line.Matches[0].Groups[1].Value
  if ((Compiler-Key $recorded) -ne (Compiler-Key $cxx)) {
    throw "$buildDir is configured with '$recorded'; remove it and re-run to build with '$cxx'"
  }
}

$toolchain = @()
if ($Compiler) {
  $resolved = Resolve-Toolchain $Compiler
  Assert-Compiler-Cache (Join-Path $eng 'recompiler/build') $resolved[1]
  if ($BuildRunner) { Assert-Compiler-Cache (Join-Path $eng 'runner/build') $resolved[1] }
  $toolchain = @("-DCMAKE_C_COMPILER=$($resolved[0])", "-DCMAKE_CXX_COMPILER=$($resolved[1])")
  Write-Host "compiler : $($resolved[0]) / $($resolved[1])"
}

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
Invoke-Native cmake -G Ninja -S (Join-Path $eng 'recompiler') -B (Join-Path $eng 'recompiler/build') "-DCMAKE_BUILD_TYPE=$Configuration" $toolchain
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
    -DNDS_BOOTSTRAP_FIRMWARE=ON $toolchain
  Invoke-Native cmake --build (Join-Path $eng 'runner/build')
}

Step 'Done'
