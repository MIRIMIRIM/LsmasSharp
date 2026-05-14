#!/usr/bin/env bash
set -euo pipefail

PREFIX="${1:?prefix is required}"
JOBS="${2:-0}"
CLEAN="${3:-0}"
REPO_URL="${4:-https://github.com/AkarinVS/FFmpeg}"
REF_NAME="${5:-lsmas}"
LSMASH_REPO_URL="${6:-}"
LSMASH_REF_NAME="${7:-}"

PREFIX="$(realpath -m "$PREFIX")"

is_full_sha() {
  echo "$1" | grep -qE '^[0-9a-fA-F]{40}$'
}

resolve_remote_ref() {
  local repo="$1"
  local ref="$2"
  local sha=""

  if is_full_sha "$ref"; then
    echo "${ref,,}"
    return 0
  fi

  sha="$(git ls-remote "$repo" "refs/heads/$ref" 2>/dev/null | awk '{print $1; exit}' || true)"
  if [ -z "$sha" ]; then
    sha="$(git ls-remote "$repo" "refs/tags/$ref^{}" 2>/dev/null | awk '{print $1; exit}' || true)"
  fi
  if [ -z "$sha" ]; then
    sha="$(git ls-remote "$repo" "refs/tags/$ref" 2>/dev/null | awk '{print $1; exit}' || true)"
  fi
  if [ -z "$sha" ]; then
    sha="$(git ls-remote "$repo" "$ref" 2>/dev/null | awk '{print $1; exit}' || true)"
  fi

  echo "$sha"
}

fetch_git_ref() {
  local dir="$1"
  local ref="$2"
  local candidate

  for candidate in "$ref" "refs/heads/$ref" "refs/tags/$ref"; do
    [ -n "$candidate" ] || continue
    if git -C "$dir" -c http.version=HTTP/1.1 fetch --depth 1 origin "$candidate"; then
      return 0
    fi
  done

  return 1
}

FFMPEG_RESOLVED_COMMIT="$(resolve_remote_ref "$REPO_URL" "$REF_NAME")"

if [ "$JOBS" -le 0 ]; then
  if command -v nproc >/dev/null 2>&1; then
    JOBS="$(nproc)"
  else
    JOBS=4
  fi
fi

# Keep build artifacts/toolchain under the target prefix drive so they persist across WSL sessions.
WORKROOT="$(dirname "$PREFIX")/_wsl"
BUILDROOT="$WORKROOT/ffmpeg-lsmas-build-local-mingw"
TOOLROOT="$WORKROOT/ffmpeg-lsmas-toolchain-local-mingw"

if [ "$CLEAN" = "1" ]; then
  rm -rf "$BUILDROOT" "$TOOLROOT"
fi

rm -rf "$PREFIX"
mkdir -p "$PREFIX"

mkdir -p "$BUILDROOT"

bootstrap_toolchain() {
  if [ -x "$TOOLROOT/usr/bin/x86_64-w64-mingw32-gcc" ] && [ -x "$TOOLROOT/usr/bin/x86_64-w64-mingw32-ld" ]; then
    return 0
  fi

  mkdir -p "$TOOLROOT"
  local dl="$BUILDROOT/debs"
  rm -rf "$dl"
  mkdir -p "$dl"
  cd "$dl"

  # Download required packages without sudo, then extract locally.
  # Note: apt-get download does not resolve dependencies, so we list the core packages explicitly.
  #
  # Some environments intermittently fail plain HTTP downloads from archive.ubuntu.com (502 via a loopback proxy).
  # Work around by using apt to resolve the .deb URI, then curl the same URI via HTTPS.
  download_deb() {
    local pkg="$1"
    local uri
    local uri_output
    uri_output="$(apt-get -o Acquire::Retries=5 --print-uris download "$pkg" 2>/dev/null || true)"
    uri="$(printf '%s\n' "$uri_output" | grep -oE "https?://[^']+" | head -n 1 || true)"
    if [ -z "$uri" ]; then
      apt-get -o Acquire::Retries=10 download "$pkg"
      return 0
    fi
    local url="$uri"
    if echo "$url" | grep -q '^http://'; then
      url="https://${url#http://}"
    fi
    local file="${url##*/}"
    if [ -f "$file" ]; then
      return 0
    fi
    curl -L --retry 10 --retry-all-errors --connect-timeout 20 -o "$file" "$url"
  }

  pkgs=(
    nasm
    ninja-build
    meson
    python3-pkg-resources
    python3-setuptools
    pkgconf-bin
    libpkgconf3
    binutils-mingw-w64-x86-64
    gcc-mingw-w64-x86-64-win32
    gcc-mingw-w64-x86-64-win32-runtime
    gcc-mingw-w64-base
    mingw-w64-common
    mingw-w64-x86-64-dev
  )

  for pkg in "${pkgs[@]}"; do
    download_deb "$pkg"
  done

  for deb in ./*.deb; do
    dpkg-deb -x "$deb" "$TOOLROOT"
  done

  # Provide un-suffixed tool names expected by FFmpeg's --cross-prefix.
  local bin="$TOOLROOT/usr/bin"
  if [ -x "$bin/x86_64-w64-mingw32-gcc-win32" ] && [ ! -e "$bin/x86_64-w64-mingw32-gcc" ]; then
    ln -s "x86_64-w64-mingw32-gcc-win32" "$bin/x86_64-w64-mingw32-gcc"
  fi
  if [ -x "$bin/x86_64-w64-mingw32-cpp-win32" ] && [ ! -e "$bin/x86_64-w64-mingw32-cpp" ]; then
    ln -s "x86_64-w64-mingw32-cpp-win32" "$bin/x86_64-w64-mingw32-cpp"
  fi
  if [ -x "$bin/x86_64-w64-mingw32-gcc-ar-win32" ] && [ ! -e "$bin/x86_64-w64-mingw32-gcc-ar" ]; then
    ln -s "x86_64-w64-mingw32-gcc-ar-win32" "$bin/x86_64-w64-mingw32-gcc-ar"
  fi
  if [ -x "$bin/x86_64-w64-mingw32-gcc-nm-win32" ] && [ ! -e "$bin/x86_64-w64-mingw32-gcc-nm" ]; then
    ln -s "x86_64-w64-mingw32-gcc-nm-win32" "$bin/x86_64-w64-mingw32-gcc-nm"
  fi
  if [ -x "$bin/x86_64-w64-mingw32-gcc-ranlib-win32" ] && [ ! -e "$bin/x86_64-w64-mingw32-gcc-ranlib" ]; then
    ln -s "x86_64-w64-mingw32-gcc-ranlib-win32" "$bin/x86_64-w64-mingw32-gcc-ranlib"
  fi
}

bootstrap_toolchain

export PATH="$TOOLROOT/usr/bin:$PATH"
# Make locally-extracted Debian Python modules visible to the system python (for meson).
export PYTHONPATH="$TOOLROOT/usr/lib/python3/dist-packages:$TOOLROOT/usr/lib/python3.12/dist-packages:${PYTHONPATH:-}"
# Make locally-extracted shared libs visible (for pkgconf, etc.).
export LD_LIBRARY_PATH="$TOOLROOT/usr/lib/x86_64-linux-gnu:$TOOLROOT/usr/lib:${LD_LIBRARY_PATH:-}"

if ! command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then
  echo "ERROR: x86_64-w64-mingw32-gcc not found after bootstrap." >&2
  exit 2
fi

# Prefer pkgconf as pkg-config (FFmpeg configure uses it for external libs like dav1d).
if command -v pkgconf >/dev/null 2>&1 && ! command -v pkg-config >/dev/null 2>&1; then
  ln -s "pkgconf" "$TOOLROOT/usr/bin/pkg-config" || true
fi
# FFmpeg's cross build prefers a target-prefixed pkg-config wrapper.
if command -v pkgconf >/dev/null 2>&1 && [ ! -e "$TOOLROOT/usr/bin/x86_64-w64-mingw32-pkg-config" ]; then
  ln -s "pkgconf" "$TOOLROOT/usr/bin/x86_64-w64-mingw32-pkg-config" || true
fi

DEPSROOT="$BUILDROOT/deps"
mkdir -p "$DEPSROOT"

build_obuparse() {
  # l-smash 2.18+ expects <obuparse.h> and -lobuparse for AV1 OBU parsing.
  local repo="https://github.com/dwbuiten/obuparse.git"
  local ref="v2.0.1"
  local src="$BUILDROOT/obuparse"
  local bld="$BUILDROOT/obuparse-build"

  rm -rf "$src" "$bld"
  if ! git -c http.version=HTTP/1.1 clone --depth 1 --branch "$ref" "$repo" "$src"; then
    rm -rf "$src"
    git -c http.version=HTTP/1.1 clone "$repo" "$src"
    (
      cd "$src"
      git -c http.version=HTTP/1.1 fetch --depth 1 origin "$ref" >/dev/null 2>&1 || true
      git checkout -f FETCH_HEAD >/dev/null 2>&1 || git checkout -f "$ref"
    )
  fi

  mkdir -p "$bld" "$DEPSROOT/include" "$DEPSROOT/lib"
  (
    cd "$bld"
    x86_64-w64-mingw32-gcc -c -O2 -std=c99 -I"$src" -o obuparse.o "$src/obuparse.c"
    x86_64-w64-mingw32-ar rcs libobuparse.a obuparse.o
    cp -f "$src/obuparse.h" "$DEPSROOT/include/"
    cp -f libobuparse.a "$DEPSROOT/lib/"
  )

  (
    cd "$src"
    git rev-parse HEAD > "$PREFIX/obuparse-commit.txt" || true
    git rev-parse --short=12 HEAD > "$PREFIX/obuparse-commit-short.txt" || true
    echo "$repo" > "$PREFIX/obuparse-remote.txt" || true
    echo "$ref" > "$PREFIX/obuparse-branch.txt" || true
  )
}

build_lsmash() {
  if [ -z "${LSMASH_REPO_URL:-}" ]; then
    return 0
  fi

  if [ -z "${LSMASH_REF_NAME:-}" ]; then
    LSMASH_REF_NAME="master"
  fi

  local repo="$LSMASH_REPO_URL"
  local ref="$LSMASH_REF_NAME"
  local src="$BUILDROOT/l-smash"

  rm -rf "$src"
  mkdir -p "$src"

  # Minimize flakiness on Windows/WSL networks by forcing HTTP/1.1.
  if ! git -c http.version=HTTP/1.1 clone --depth 1 --branch "$ref" "$repo" "$src"; then
    rm -rf "$src"
    git -c http.version=HTTP/1.1 clone "$repo" "$src"
    (
      cd "$src"
      git -c http.version=HTTP/1.1 fetch --depth 1 origin "$ref" >/dev/null 2>&1 || true
      git checkout -f FETCH_HEAD >/dev/null 2>&1 || git checkout -f "$ref"
    )
  fi

  (
    cd "$src"
    # l-smash uses a custom configure script (not autoconf).
    if [ -x ./configure ]; then
      build_obuparse
      ./configure \
        --prefix="$DEPSROOT" \
        --target-os=mingw32 \
        --cross-prefix=x86_64-w64-mingw32- \
        --extra-cflags="-I$DEPSROOT/include" \
        --extra-ldflags="-L$DEPSROOT/lib"
    fi
    make -j"$JOBS"
    make install
  )

  (
    cd "$src"
    git rev-parse HEAD > "$PREFIX/lsmash-commit.txt" || true
    git rev-parse --short=12 HEAD > "$PREFIX/lsmash-commit-short.txt" || true
    echo "$repo" > "$PREFIX/lsmash-remote.txt" || true
    echo "$ref" > "$PREFIX/lsmash-branch.txt" || true
  )
}

build_dav1d() {
  local ver="1.5.3"
  if [ -f "$PREFIX/dav1d-version.txt" ] && [ "$(cat "$PREFIX/dav1d-version.txt" 2>/dev/null || true)" = "$ver" ] && [ -f "$DEPSROOT/lib/pkgconfig/dav1d.pc" ]; then
    return 0
  fi
  if ! command -v meson >/dev/null 2>&1 || ! command -v ninja >/dev/null 2>&1; then
    echo "ERROR: meson/ninja not found; cannot build dav1d." >&2
    exit 4
  fi

  local tar="$BUILDROOT/dav1d-$ver.tar.gz"
  local url="https://github.com/videolan/dav1d/archive/refs/tags/$ver.tar.gz"
  local src="$BUILDROOT/dav1d-$ver"
  local bld="$BUILDROOT/dav1d-build-$ver"
  local cross="$BUILDROOT/mingw-cross.txt"

  # Download (with resume) and extract.
  rm -rf "$src" "$bld"
  if command -v curl >/dev/null 2>&1; then
    curl -L --retry 5 --retry-all-errors --connect-timeout 20 -C - -o "$tar" "$url"
  else
    wget -O "$tar" "$url"
  fi
  tar -xzf "$tar" -C "$BUILDROOT"

  cat > "$cross" <<EOF
[binaries]
c = 'x86_64-w64-mingw32-gcc'
cpp = 'x86_64-w64-mingw32-g++'
ar = 'x86_64-w64-mingw32-gcc-ar'
strip = 'x86_64-w64-mingw32-strip'
windres = 'x86_64-w64-mingw32-windres'

[properties]
needs_exe_wrapper = true

[host_machine]
system = 'windows'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'
EOF

  meson setup "$bld" "$src" \
    --cross-file "$cross" \
    --default-library=static \
    --buildtype=release \
    --prefix="$DEPSROOT"
  ninja -C "$bld" install

  echo "$ver" > "$PREFIX/dav1d-version.txt"
}

build_zlib() {
  local ver="1.3.1"
  if [ -f "$PREFIX/zlib-version.txt" ] && [ "$(cat "$PREFIX/zlib-version.txt" 2>/dev/null || true)" = "$ver" ] && [ -f "$DEPSROOT/lib/libz.a" ] && [ -f "$DEPSROOT/include/zlib.h" ]; then
    return 0
  fi

  local tar="$BUILDROOT/zlib-$ver.tar.gz"
  local url="https://github.com/madler/zlib/archive/refs/tags/v$ver.tar.gz"
  local src="$BUILDROOT/zlib-$ver"

  rm -rf "$src"
  if command -v curl >/dev/null 2>&1; then
    curl -L --retry 5 --retry-all-errors --connect-timeout 20 -C - -o "$tar" "$url"
  else
    wget -O "$tar" "$url"
  fi
  tar -xzf "$tar" -C "$BUILDROOT"

  # zlib's configure supports cross compilation reasonably well if CC/AR/RANLIB are provided.
  (
    cd "$src"
    CC=x86_64-w64-mingw32-gcc \
    AR=x86_64-w64-mingw32-ar \
    RANLIB=x86_64-w64-mingw32-ranlib \
      ./configure --static --prefix="$DEPSROOT"
    make -j"$JOBS"
    make install
  )

  echo "$ver" > "$PREFIX/zlib-version.txt"
}

cd "$BUILDROOT"
if [ ! -d FFmpeg/.git ]; then
  rm -rf FFmpeg
  git -c init.defaultBranch=ci init FFmpeg
  git -C FFmpeg remote add origin "$REPO_URL"
fi

cd FFmpeg
git remote get-url origin >/dev/null 2>&1 || git remote add origin "$REPO_URL"
git remote set-url origin "$REPO_URL" >/dev/null 2>&1 || true
fetch_ref="$REF_NAME"
if [ -n "${FFMPEG_RESOLVED_COMMIT:-}" ]; then
  fetch_ref="$FFMPEG_RESOLVED_COMMIT"
fi
fetch_git_ref "." "$fetch_ref" || fetch_git_ref "." "$REF_NAME"
git checkout -f FETCH_HEAD
git rev-parse HEAD > "$PREFIX/lsmas-commit.txt"
git rev-parse --short=12 HEAD > "$PREFIX/lsmas-commit-short.txt"
git config --get remote.origin.url > "$PREFIX/lsmas-remote.txt" || true
echo "$REF_NAME" > "$PREFIX/lsmas-branch.txt" || true

make distclean >/dev/null 2>&1 || true

# Build and expose l-smash (optional; used by L-SMASH-Works for MP4/qt indexing).
build_lsmash
# Build and expose dav1d for --enable-libdav1d.
build_dav1d
# Build and expose zlib for --enable-zlib.
build_zlib
export PKG_CONFIG_PATH="$DEPSROOT/lib/pkgconfig"
export PKG_CONFIG_LIBDIR="$DEPSROOT/lib/pkgconfig"

# Some forks drop/rename configure flags. Probe optional ones.
HAS_DISABLE_POSTPROC=0
if ./configure --help 2>/dev/null | grep -q -- '--disable-postproc'; then
  HAS_DISABLE_POSTPROC=1
fi

EXTRA_CONFIG_OPTS=()
if [ "$HAS_DISABLE_POSTPROC" = "1" ]; then
  EXTRA_CONFIG_OPTS+=(--disable-postproc)
fi

# Use relative paths so avcodec_configuration() does not embed absolute /mnt/... paths.
./configure \
  --prefix="/" \
  --arch=x86_64 \
  --target-os=mingw32 \
  --cross-prefix=x86_64-w64-mingw32- \
  --enable-gpl --enable-version3 --enable-static --disable-shared \
  --enable-avcodec --enable-avformat --enable-swscale --enable-swresample \
  --disable-programs --disable-avdevice --disable-avfilter \
  --disable-encoders --disable-muxers --disable-doc --disable-debug \
  --disable-autodetect \
  --extra-cflags="-I../deps/include" \
  --extra-ldflags="-L../deps/lib" \
  --enable-libdav1d \
  --enable-zlib \
  "${EXTRA_CONFIG_OPTS[@]}"

make -j"$JOBS"
make install DESTDIR="$PREFIX"

# FFmpeg's libavcodec will reference dav1d symbols, but the static archive lives outside the
# FFmpeg install prefix. Copy it into the prefix so downstream linkers (e.g. zig) can resolve it.
mkdir -p "$PREFIX/lib" "$PREFIX/include"
cp -f "$DEPSROOT/lib/libdav1d.a" "$PREFIX/lib/" || true
cp -rf "$DEPSROOT/include/dav1d" "$PREFIX/include/" || true
cp -f "$DEPSROOT/lib/libz.a" "$PREFIX/lib/" || true
cp -f "$DEPSROOT/include/zlib.h" "$PREFIX/include/" || true
cp -f "$DEPSROOT/include/zconf.h" "$PREFIX/include/" || true
if [ -f "$DEPSROOT/lib/libobuparse.a" ]; then
  cp -f "$DEPSROOT/lib/libobuparse.a" "$PREFIX/lib/" || true
fi
if [ -f "$DEPSROOT/include/obuparse.h" ]; then
  cp -f "$DEPSROOT/include/obuparse.h" "$PREFIX/include/" || true
fi
if [ -f "$DEPSROOT/lib/liblsmash.a" ]; then
  cp -f "$DEPSROOT/lib/liblsmash.a" "$PREFIX/lib/" || true
fi
if [ -f "$DEPSROOT/include/lsmash.h" ]; then
  cp -f "$DEPSROOT/include/lsmash.h" "$PREFIX/include/" || true
fi
