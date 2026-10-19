#!/usr/bin/env sh
# Bootstrap + build the ndsrecomp recompiler + runner (pinned git submodule).
#
# Mirror of tools/bootstrap_ndsrecomp.ps1 for Linux/macOS.
#
# Usage:
#   sh tools/bootstrap_ndsrecomp.sh
#   sh tools/bootstrap_ndsrecomp.sh --build-runner
#   sh tools/bootstrap_ndsrecomp.sh --build-runner --sdl3
#   sh tools/bootstrap_ndsrecomp.sh --compiler clang
#   sh tools/bootstrap_ndsrecomp.sh --compiler /opt/llvm/bin/clang
#
# --compiler picks the toolchain for the recompiler (and, with --build-runner,
# the runner): a name resolved from PATH (clang, clang-18, gcc) or a path to
# the C compiler. The C++ sibling is derived (clang -> clang++, gcc -> g++) and,
# for a path, must sit next to it. Switching compilers on an existing build
# directory is refused -- remove third_party/ndsrecomp/recompiler/build (and
# runner/build) first; CMake caches the compiler choice.
set -eu

build_runner=0
sdl3=0
compiler=
while [ $# -gt 0 ]; do
  case "$1" in
    --build-runner) build_runner=1; shift ;;
    --sdl3) sdl3=1; shift ;;
    --compiler) compiler=$2; shift 2 ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done

# The C++ compiler is the C compiler's sibling (clang -> clang++, gcc -> g++);
# 'cl' serves both. Anything else is refused rather than guessed.
cxx_of() {
  case "$1" in
    */*) dir=${1%/*}/; file=${1##*/} ;;
    *) dir=; file=$1 ;;
  esac
  case "$file" in
    *.*) stem=${file%.*}; ext=.${file##*.} ;;
    *) stem=$file; ext= ;;
  esac
  case "$stem" in
    clang*) cxx=clang++${stem#clang} ;;
    gcc*) cxx=g++${stem#gcc} ;;
    cl) cxx=cl ;;
    *) echo "don't know the C++ sibling of '$file'; pass a clang/gcc-family compiler" >&2; exit 2 ;;
  esac
  echo "$dir$cxx$ext"
}

# CMake caches the compiler, and re-pointing it makes a stale build dir fail in
# opaque ways. Refuse the mismatch instead of letting CMake guess. Only the
# executable's name matters (a bare 'clang++' and a full path to clang++.exe are
# the same compiler; 'clang-18' is not).
assert_compiler_cache() {
  [ -n "$compiler" ] || return 0
  [ -f "$1/CMakeCache.txt" ] || return 0
  recorded=$(sed -n 's/^CMAKE_CXX_COMPILER:[^=]*=//p' "$1/CMakeCache.txt" | head -n 1)
  [ -n "$recorded" ] || return 0
  recorded_key=${recorded##*/}; recorded_key=${recorded_key%.exe}
  cxx_key=${cxx##*/}; cxx_key=${cxx_key%.exe}
  if [ "$recorded_key" != "$cxx_key" ]; then
    echo "$1 is configured with '$recorded'; remove it and re-run to build with '$cxx'" >&2
    exit 1
  fi
}

cxx=
if [ -n "$compiler" ]; then
  case "$compiler" in
    */*) [ -x "$compiler" ] || { echo "no such compiler: $compiler" >&2; exit 2; } ;;
  esac
  cxx=$(cxx_of "$compiler")
  case "$compiler" in
    */*) [ -x "$cxx" ] || { echo "C++ compiler not found next to it: $cxx" >&2; exit 2; } ;;
  esac
  echo "compiler : $compiler / $cxx"
fi

case "$0" in
  */*) here=$(cd "${0%/*}" && pwd) ;;
  *)   here=$(pwd) ;;
esac
root=$(cd "$here/.." && pwd)
eng="$root/third_party/ndsrecomp"
gen="$eng/generated"

step() { printf '\033[36m==> %s\033[0m\n' "$1"; }

step "Init ndsrecomp submodule"
git -C "$root" submodule update --init -- third_party/ndsrecomp

# The v0.6.10 nested submodule URL is relative and broken; point it at GitHub.
if [ ! -f "$eng/external/arm-recomp-core/CMakeLists.txt" ]; then
  git -C "$eng" config submodule.external/arm-recomp-core.url https://github.com/mstan/arm-recomp-core.git
fi
git -C "$eng" submodule update --init --recursive

step "Stage toml++ single header"
toml="$eng/recompiler/third_party/toml.hpp"
if [ ! -f "$toml" ]; then
  mkdir -p "$(dirname "$toml")"
  tmp=$(mktemp -d)
  git clone --depth 1 --branch v3.4.0 https://github.com/marzer/tomlplusplus.git "$tmp"
  cp "$tmp/toml.hpp" "$toml"
  rm -rf "$tmp"
fi

step "Build recompiler"
set --
if [ -n "$compiler" ]; then
  assert_compiler_cache "$eng/recompiler/build"
  set -- "-DCMAKE_C_COMPILER=$compiler" "-DCMAKE_CXX_COMPILER=$cxx"
fi
cmake -G Ninja -S "$eng/recompiler" -B "$eng/recompiler/build" -DCMAKE_BUILD_TYPE=Release "$@"
cmake --build "$eng/recompiler/build"

step "Generate BIOS banks"
rc="$eng/recompiler/build/nds_recompile"
f9="$eng/third_party/freebios/drastic_bios_arm9.bin"
f7="$eng/third_party/freebios/drastic_bios_arm7.bin"
c9="$eng/bios/freebios9.toml"
c7="$eng/bios/freebios7.toml"
"$rc" --config "$c9" --bin "$f9" --out "$gen" --bank freebios_arm9
"$rc" --config "$c7" --bin "$f7" --out "$gen" --bank freebios_arm7
# Retail BIOS banks must exist to link, but no retail dump is shipped; emit
# FreeBIOS-content placeholders. The runtime uses the FreeBIOS banks by default.
"$rc" --config "$c9" --bin "$f9" --out "$gen" --bank arm9_bios
"$rc" --config "$c7" --bin "$f7" --out "$gen" --bank arm7_bios

if [ "$build_runner" -eq 1 ]; then
  step "Configure + build runner"
  if [ "$sdl3" -eq 1 ]; then backend=SDL3; compute=ON; else backend=NONE; compute=OFF; fi
  set --
  if [ -n "$compiler" ]; then
    assert_compiler_cache "$eng/runner/build"
    set -- "-DCMAKE_C_COMPILER=$compiler" "-DCMAKE_CXX_COMPILER=$cxx"
  fi
  cmake -G Ninja -S "$eng/runner" -B "$eng/runner/build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DNDS_SDL_BACKEND="$backend" \
    -DNDS_ENABLE_COMPUTE_RENDERER="$compute" \
    -DNDS_BOOTSTRAP_FIRMWARE=ON "$@"
  cmake --build "$eng/runner/build"
fi

step "Done"
