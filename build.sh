#!/usr/bin/env bash
set -euo pipefail

Usage() {
  cat <<'EOF'
SmartStreamSlicer all-in-one build script (POSIX).

  ./build.sh [-static] [--smoke] [--ragel <path>] [--boost-root <dir>]
             [--build-dir <dir>] [-j <n>]

  -static      Link bundled libraries statically (default: shared build).
  --smoke      Run the test suite (tests/smoke.sh) against the new binary.
  --ragel      Path to ragel if it is not on PATH.
  --boost-root Use an existing Boost install instead of downloading one. It is
               verified to be exactly Boost 1.84 (boost/version.hpp check).
  --build-dir  Output directory (default: build_linux for -static,
               build_linux_shared otherwise). It is removed and
               recreated, so each run is a clean build.
  -j           Number of parallel jobs (default: machine maximum, i.e. all cores).

The script ensures prerequisites (git submodules, CMake 3.18+, a C++ compiler,
ragel, Boost 1.84 headers) are present - downloading and hash-verifying what is
missing - then drives the CMake build, which remains the build backend.
Downloads (Boost) go to a temp-dir cache (override: SSS_CACHE), so reboots
clean them up automatically and each run re-verifies what it uses.
EOF
  exit 0
}

STATIC=OFF
DO_SMOKE=0
RAGEL_PATH=""
BOOST_ROOT_DIR=""
BUILD_DIR=""
JOBS=""

while (($#)); do
  case "$1" in
    -static) STATIC=ON; shift ;;
    --smoke) DO_SMOKE=1; shift ;;
    --ragel) RAGEL_PATH="$2"; shift 2 ;;
    -ragel) RAGEL_PATH="$2"; shift 2 ;;
    --boost-root) BOOST_ROOT_DIR="$2"; shift 2 ;;
    --build-dir) BUILD_DIR="$2"; shift 2 ;;
    -j) JOBS="$2"; shift 2 ;;
    -j*) JOBS="${1#-j}"; shift ;;
    -h|--help|-help) Usage ;;
    *) echo "unknown option: $1"; Usage ;;
  esac
done

# Flavor-specific defaults keep Linux builds out of Windows' build/ dir:
# static -> build_linux, shared -> build_linux_shared.
if [ -z "$BUILD_DIR" ]; then
  BUILD_DIR="build_linux"
  if [ "$STATIC" = OFF ]; then BUILD_DIR="build_linux_shared"; fi
fi

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$REPO_ROOT"

fail() { echo "error: $*" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }

echo "[*] SmartStreamSlicer all-in-one build (POSIX)"

have git || fail "git is required to fetch the vendored submodules (fastcdc, vectorscan)."
have cmake || fail "cmake not found. Install CMake 3.18+ (e.g. sudo apt-get install cmake)."
have g++ || fail "g++ not found. Install a C++ toolchain (e.g. sudo apt-get install g++)."
have gcc || fail "gcc not found."

CMAKE_VER="$(cmake --version | head -1)"
if [[ ! "$CMAKE_VER" =~ cmake\ version\ ([0-9]+)\.([0-9]+) ]]; then
  fail "unexpected cmake output: $CMAKE_VER"
fi
CMAKE_MAJOR="${BASH_REMATCH[1]}"
CMAKE_MINOR="${BASH_REMATCH[2]}"
if (( CMAKE_MAJOR < 3 )) || (( CMAKE_MAJOR == 3 && CMAKE_MINOR < 18 )); then
  fail "cmake $CMAKE_MAJOR.$CMAKE_MINOR is too old; need 3.18+."
fi
echo "[ok] cmake $CMAKE_MAJOR.$CMAKE_MINOR"

echo "[*] initializing submodules (shallow, non-recursive)..."
git submodule update --init --depth 1
git -C "$REPO_ROOT/thirdparty/vectorscan" submodule update --init --depth 1

if [ -n "$RAGEL_PATH" ]; then
  :
elif have ragel; then
  RAGEL_PATH="$(command -v ragel)"
else
  echo "[..] ragel not on PATH; attempting to install it..."
  if [ "$(id -u)" = 0 ]; then
    if have apt-get; then apt-get install -y ragel; fi
  elif have apt-get && sudo -n true 2>/dev/null; then
    sudo -n apt-get install -y ragel
  elif have brew; then
    brew install ragel
  elif have pacman; then
    sudo -n pacman -S --noconfirm ragel 2>/dev/null || true
  fi
  have ragel || fail "ragel still missing. Install it or pass --ragel <path>."
  RAGEL_PATH="$(command -v ragel)"
fi
"$RAGEL_PATH" -v >/dev/null 2>&1 || fail "ragel at '$RAGEL_PATH' failed to run."
echo "[ok] ragel: $RAGEL_PATH"

CACHE_DIR="${SSS_CACHE:-${TMPDIR:-/tmp}/smartstreamslicer}"
LEGACY_CACHE="$HOME/.cache/smartstreamslicer"
if [ "$CACHE_DIR" != "$LEGACY_CACHE" ] && [ -d "$LEGACY_CACHE" ]; then
  echo "[..] removing legacy cache dir $LEGACY_CACHE (cache now lives in the OS temp dir)..."
  rm -rf "$LEGACY_CACHE"
fi
BOOST_TGZ="$CACHE_DIR/boost_1_84_0.tar.gz"
BOOST_SHA256="a5800f405508f5df8114558ca9855d2640a2de8f0445f051fa1c7c3383045724"
BOOST_URL="https://archives.boost.io/release/1.84.0/source/boost_1_84_0.tar.gz"
BOOST_DIR="$CACHE_DIR/boost_1_84_0"
BOOST_ARGS=()

hashtest() {
  if have sha256sum; then sha256sum "$1" | awk '{print $1}'
  else shasum -a 256 "$1" | awk '{print $1}'
  fi
}

if [ -n "$BOOST_ROOT_DIR" ]; then
  BOOST_ROOT_DIR="$(cd "$BOOST_ROOT_DIR" && pwd)"
  [ -f "$BOOST_ROOT_DIR/boost/version.hpp" ] || fail "BOOST_ROOT '$BOOST_ROOT_DIR' has no boost/version.hpp."
  grep -q 'BOOST_VERSION 108400' "$BOOST_ROOT_DIR/boost/version.hpp" || fail "BOOST_ROOT '$BOOST_ROOT_DIR' is not Boost 1.84 (expected BOOST_VERSION 108400)."
  echo "[ok] boost: using $BOOST_ROOT_DIR"
  BOOST_ARGS+=("-DBOOST_ROOT=$BOOST_ROOT_DIR")
else
  mkdir -p "$CACHE_DIR"
  if [ ! -f "$BOOST_DIR/boost/version.hpp" ]; then
    if [ -f "$BOOST_TGZ" ]; then
      if [ "$(hashtest "$BOOST_TGZ")" = "$BOOST_SHA256" ]; then
        echo "[ok] boost: cached tarball verified (SHA256 $BOOST_SHA256)"
      else
        echo "[..] cached boost tarball SHA256 mismatch; re-downloading..."
        rm -f "$BOOST_TGZ"
      fi
    fi
    if [ ! -f "$BOOST_TGZ" ]; then
      echo "[..] downloading boost 1.84.0 headers..."
      if have curl; then
        curl -fSL -o "$BOOST_TGZ" "$BOOST_URL"
      elif have wget; then
        wget -O "$BOOST_TGZ" "$BOOST_URL"
      else
        fail "need curl or wget to download boost."
      fi
      [ "$(hashtest "$BOOST_TGZ")" = "$BOOST_SHA256" ] || fail "downloaded boost tarball SHA256 mismatch."
    fi
    rm -rf "$BOOST_DIR" 2>/dev/null || true
    echo "[..] extracting boost headers into $CACHE_DIR..."
    tar -xzf "$BOOST_TGZ" -C "$CACHE_DIR"
    [ -f "$BOOST_DIR/boost/version.hpp" ] || fail "boost extraction produced no boost/version.hpp."
  fi
  echo "[ok] boost: headers in $BOOST_DIR"
  BOOST_ARGS+=("-DBOOST_ROOT=$BOOST_DIR")
fi

[ -e "$BUILD_DIR" ] && rm -rf "$BUILD_DIR"

GENERATOR="Unix Makefiles"
if have ninja; then GENERATOR="Ninja"; fi

if [ -z "$JOBS" ]; then
  if have nproc; then
    JOBS="$(nproc)"
  elif have getconf; then
    JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)"
  else
    JOBS=1
  fi
fi

echo "[*] configuring (SSS_STATIC=$STATIC) via cmake..."
cmake -S "$REPO_ROOT" -B "$BUILD_DIR" -G "$GENERATOR" \
  -DCMAKE_BUILD_TYPE=Release "-DSSS_STATIC=$STATIC" "-DRAGEL=$RAGEL_PATH" \
  "${BOOST_ARGS[@]}"

echo "[*] building (parallel jobs: $JOBS)..."
cmake --build "$BUILD_DIR" -j "$JOBS"

BIN="$BUILD_DIR/sss"
[ -x "$BIN" ] || fail "expected binary not found: $BIN"
echo "[*] binary:"
"$BIN" --version

if [ "$DO_SMOKE" = 1 ]; then
  WD="$(mktemp -d)"
  echo "[*] running smoke tests..."
  SMOKE_MANIFEST_DIR="$WD" bash "$REPO_ROOT/tests/smoke.sh" "$BIN" "$WD"
fi

echo "OK: $BIN"