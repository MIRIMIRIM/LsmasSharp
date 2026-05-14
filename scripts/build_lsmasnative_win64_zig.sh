#!/usr/bin/env bash
set -euo pipefail

LSMAS_WORKS_DIR="build-deps/L-SMASH-Works"
LSMAS_WORKS_REPO="https://github.com/AkarinVS/L-SMASH-Works.git"
LSMAS_WORKS_REF="master"
LSMAS_WORKS_REF_LABEL=""
FFMPEG_PREFIX="build-deps/ffmpeg-lsmas-win64-local-mingw-static"
OUT_DIR="artifacts/native/win-x64"
OPTIMIZE="Debug"

usage() {
  cat <<'EOF'
Usage: scripts/build_lsmasnative_win64_zig.sh [options]

Options:
  --lsmas-works-dir PATH       L-SMASH-Works checkout directory.
  --lsmas-works-repo URL       L-SMASH-Works upstream repository.
  --lsmas-works-ref REF        Branch, tag, or commit to checkout.
  --lsmas-works-ref-label REF  Ref label embedded into version metadata.
  --ffmpeg-prefix PATH         FFmpeg install prefix containing include/ and lib/.
  --out-dir PATH               Output directory for lsmasnative.dll.
  --release                    Build with -Doptimize=ReleaseFast.
EOF
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --lsmas-works-dir) LSMAS_WORKS_DIR="${2:?}"; shift 2 ;;
    --lsmas-works-repo) LSMAS_WORKS_REPO="${2:?}"; shift 2 ;;
    --lsmas-works-ref) LSMAS_WORKS_REF="${2:?}"; shift 2 ;;
    --lsmas-works-ref-label) LSMAS_WORKS_REF_LABEL="${2:?}"; shift 2 ;;
    --ffmpeg-prefix) FFMPEG_PREFIX="${2:?}"; shift 2 ;;
    --out-dir) OUT_DIR="${2:?}"; shift 2 ;;
    --release) OPTIMIZE="ReleaseFast"; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
  esac
done

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

resolve_repo_path() {
  case "$1" in
    /*) realpath -m "$1" ;;
    *) realpath -m "$REPO_ROOT/$1" ;;
  esac
}

LSMAS_WORKS_DIR_ABS="$(resolve_repo_path "$LSMAS_WORKS_DIR")"
FFMPEG_PREFIX_ABS="$(resolve_repo_path "$FFMPEG_PREFIX")"
OUT_DIR_ABS="$(resolve_repo_path "$OUT_DIR")"
NATIVE_DIR="$REPO_ROOT/src/LsmasNative"

if [ ! -d "$FFMPEG_PREFIX_ABS" ]; then
  echo "FFmpeg prefix does not exist: $FFMPEG_PREFIX_ABS" >&2
  exit 3
fi

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

  echo "Could not fetch ref '$ref' in $dir" >&2
  return 1
}

ensure_git_checkout() {
  local dir="$1"
  local repo="$2"
  local ref="$3"

  if [ -z "$ref" ]; then
    ref="master"
  fi

  if [ ! -d "$dir/.git" ]; then
    if [ -e "$dir" ] && [ -n "$(find "$dir" -mindepth 1 -maxdepth 1 -print -quit)" ]; then
      echo "Dependency directory exists but is not a git checkout: $dir" >&2
      exit 4
    fi

    mkdir -p "$(dirname "$dir")"
    git -c http.version=HTTP/1.1 clone --no-checkout "$repo" "$dir"
  fi

  git -C "$dir" remote set-url origin "$repo"
  fetch_git_ref "$dir" "$ref"
  git -C "$dir" checkout -f FETCH_HEAD
  git -C "$dir" submodule update --init --recursive --depth 1
}

read_text_file() {
  local file="$1"
  if [ -f "$file" ]; then
    tr -d '\r\n' < "$file"
  fi
}

shorten_commit() {
  local value="$1"
  if [ "${#value}" -gt 12 ]; then
    echo "${value:0:12}"
  else
    echo "$value"
  fi
}

echo "Building lsmasnative via zig"
echo "  L-SMASH-WORKS: $LSMAS_WORKS_DIR_ABS"
echo "  LSW repo/ref:   $LSMAS_WORKS_REPO @ $LSMAS_WORKS_REF"
echo "  FFmpeg prefix:  $FFMPEG_PREFIX_ABS"
echo "  Output:         $OUT_DIR_ABS"

ensure_git_checkout "$LSMAS_WORKS_DIR_ABS" "$LSMAS_WORKS_REPO" "$LSMAS_WORKS_REF"
mkdir -p "$OUT_DIR_ABS"

LSW_GIT_URL="$(git -C "$LSMAS_WORKS_DIR_ABS" config --get remote.origin.url 2>/dev/null || true)"
LSW_GIT_HEAD="$(git -C "$LSMAS_WORKS_DIR_ABS" rev-parse --short=12 HEAD 2>/dev/null || true)"
LSW_GIT_BRANCH="$LSMAS_WORKS_REF_LABEL"
if [ -z "$LSW_GIT_BRANCH" ]; then
  LSW_GIT_BRANCH="$(git -C "$LSMAS_WORKS_DIR_ABS" rev-parse --abbrev-ref HEAD 2>/dev/null || true)"
fi
if [ -z "$LSW_GIT_BRANCH" ] || [ "$LSW_GIT_BRANCH" = "HEAD" ]; then
  LSW_GIT_BRANCH="$LSMAS_WORKS_REF"
fi

LSMAS_NATIVE_GIT_HEAD="$(git -C "$REPO_ROOT" rev-parse --short=12 HEAD 2>/dev/null || true)"
LSMAS_NATIVE_VERSION="$(git -C "$REPO_ROOT" describe --tags --always --dirty 2>/dev/null || true)"
ZIG_VERSION="$(zig version 2>/dev/null || true)"

FFMPEG_GIT_URL="$(read_text_file "$FFMPEG_PREFIX_ABS/lsmas-remote.txt")"
FFMPEG_GIT_HEAD="$(read_text_file "$FFMPEG_PREFIX_ABS/lsmas-commit-short.txt")"
FFMPEG_GIT_BRANCH="$(read_text_file "$FFMPEG_PREFIX_ABS/lsmas-branch.txt")"
if [ -z "$FFMPEG_GIT_HEAD" ]; then
  FFMPEG_GIT_HEAD="$(shorten_commit "$(read_text_file "$FFMPEG_PREFIX_ABS/lsmas-commit.txt")")"
fi

DAV1D_VERSION="$(read_text_file "$FFMPEG_PREFIX_ABS/dav1d-version.txt")"
ZLIB_VERSION="$(read_text_file "$FFMPEG_PREFIX_ABS/zlib-version.txt")"

LSW_GIT_URL="${LSW_GIT_URL:-$LSMAS_WORKS_REPO}"
LSW_GIT_HEAD="${LSW_GIT_HEAD:-unknown}"
LSW_GIT_BRANCH="${LSW_GIT_BRANCH:-unknown}"
LSMAS_NATIVE_GIT_HEAD="${LSMAS_NATIVE_GIT_HEAD:-unknown}"
LSMAS_NATIVE_VERSION="${LSMAS_NATIVE_VERSION:-0.0.0-dev}"
ZIG_VERSION="${ZIG_VERSION:-unknown}"
FFMPEG_GIT_URL="${FFMPEG_GIT_URL:-https://github.com/AkarinVS/FFmpeg}"
FFMPEG_GIT_HEAD="${FFMPEG_GIT_HEAD:-unknown}"
FFMPEG_GIT_BRANCH="${FFMPEG_GIT_BRANCH:-lsmas}"
DAV1D_VERSION="${DAV1D_VERSION:-unknown}"
ZLIB_VERSION="${ZLIB_VERSION:-unknown}"

LSW_FOR_ZIG="$(realpath --relative-to="$NATIVE_DIR" "$LSMAS_WORKS_DIR_ABS")"
FFMPEG_FOR_ZIG="$(realpath --relative-to="$NATIVE_DIR" "$FFMPEG_PREFIX_ABS")"

(
  cd "$NATIVE_DIR"
  zig build \
    -Dtarget=x86_64-windows-gnu \
    "-Doptimize=$OPTIMIZE" \
    -Dffmpeg_static=true \
    "-Dffmpeg_prefix=$FFMPEG_FOR_ZIG" \
    "-Dl_smash_works_dir=$LSW_FOR_ZIG" \
    "-Dlsmasnative_version=$LSMAS_NATIVE_VERSION" \
    "-Dlsmasnative_zig_version=$ZIG_VERSION" \
    "-Dlsmasnative_git_head=$LSMAS_NATIVE_GIT_HEAD" \
    "-Dlsmashworks_git_url=$LSW_GIT_URL" \
    "-Dlsmashworks_git_head=$LSW_GIT_HEAD" \
    "-Dlsmashworks_git_branch=$LSW_GIT_BRANCH" \
    "-Dffmpeg_git_url=$FFMPEG_GIT_URL" \
    "-Dffmpeg_git_head=$FFMPEG_GIT_HEAD" \
    "-Dffmpeg_git_branch=$FFMPEG_GIT_BRANCH" \
    "-Ddav1d_version=$DAV1D_VERSION" \
    "-Dzlib_version=$ZLIB_VERSION"
)

cp -f "$NATIVE_DIR/zig-out/bin/lsmasnative.dll" "$OUT_DIR_ABS/"
if [ -f "$NATIVE_DIR/zig-out/bin/lsmasnative.pdb" ]; then
  cp -f "$NATIVE_DIR/zig-out/bin/lsmasnative.pdb" "$OUT_DIR_ABS/"
fi

echo "Done. Native binaries in: $OUT_DIR_ABS"
