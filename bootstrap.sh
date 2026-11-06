#!/bin/bash

#
#  bootstrap.sh
#  AuxDim
#
#  Fetches the build dependencies into Deps/. Needs only git, curl and unzip, all part
#  of the Xcode Command Line Tools (or any Linux distribution).
#
#    Deps/MacKernelSDK/   kernel headers and libkmod.a        (acidanthera/MacKernelSDK, git clone)
#    Deps/Lilu.kext/      Lilu plugin SDK: headers + library  (acidanthera/Lilu, released DEBUG build)
#
#  Usage:
#    ./bootstrap.sh                       fetch what is missing, Lilu = latest release
#    LILU_VERSION=1.7.1 ./bootstrap.sh    pin the Lilu release (use the one you boot with)
#    ./bootstrap.sh --force               delete the dependencies and fetch them again
#    ./bootstrap.sh --clean               delete the dependencies and stop
#
#  Build against the same Lilu release you run on the target machine: a plugin compiled
#  against a newer Lilu API can refuse to load on an older Lilu.
#

set -euo pipefail

cd "$(dirname "$0")"

DEPS="Deps"
LILU_REPO="acidanthera/Lilu"
SDK_REPO="https://github.com/acidanthera/MacKernelSDK.git"
LILU_VERSION="${LILU_VERSION:-}"

usage() {
  sed -n '/^#  Usage:/,/^#$/p' "$0" | sed '$d' | sed 's/^#  \{0,1\}//'
}

clean_deps() {
  # Also removes the copies an earlier version of this script left in the project root.
  rm -rf "$DEPS/MacKernelSDK" "$DEPS/Lilu.kext" MacKernelSDK Lilu.kext
  rmdir "$DEPS" 2>/dev/null || true
}

ACTION="fetch"
case "${1:-}" in
  "")            ;;
  --force)       ACTION="force" ;;
  --clean)       ACTION="clean" ;;
  -h|--help)     usage; exit 0 ;;
  *)             echo "ERROR: unknown option '$1'"; usage; exit 1 ;;
esac

if [ "$ACTION" = "clean" ]; then
  echo "Removing $DEPS (MacKernelSDK, Lilu.kext)..."
  clean_deps
  echo "Done."
  exit 0
fi

for tool in git curl unzip; do
  command -v "$tool" >/dev/null 2>&1 || {
    echo "ERROR: '$tool' not found. On macOS run: xcode-select --install"
    exit 1
  }
done

if [ "$ACTION" = "force" ]; then
  echo "Removing the current dependencies..."
  clean_deps
fi

mkdir -p "$DEPS"

# Earlier versions fetched into the project root: move those into Deps/ instead of downloading again.
for legacy in MacKernelSDK Lilu.kext; do
  if [ -d "$legacy" ] && [ ! -e "$DEPS/$legacy" ]; then
    echo "Moving $legacy to $DEPS/..."
    mv "$legacy" "$DEPS/$legacy"
  fi
done

# ---- MacKernelSDK -------------------------------------------------------------

if [ -f "$DEPS/MacKernelSDK/Library/x86_64/libkmod.a" ]; then
  echo "MacKernelSDK already present, skipping."
else
  echo "Fetching MacKernelSDK..."
  rm -rf "$DEPS/MacKernelSDK"
  git clone --depth 1 "$SDK_REPO" "$DEPS/MacKernelSDK"
  [ -f "$DEPS/MacKernelSDK/Library/x86_64/libkmod.a" ] || { echo "ERROR: MacKernelSDK has no Library/x86_64/libkmod.a"; exit 1; }
fi

# ---- Lilu ---------------------------------------------------------------------

latest_lilu_version() {
  local ver=""
  # 1) the "latest release" redirect ends in .../releases/tag/<version>
  ver="$(curl -fsSIL -o /dev/null -w '%{url_effective}' "https://github.com/$LILU_REPO/releases/latest" 2>/dev/null | sed 's|.*/||' || true)"
  # 2) fall back to the highest x.y.z tag
  if ! [[ "$ver" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    ver="$(git ls-remote --tags --refs "https://github.com/$LILU_REPO.git" 2>/dev/null \
      | sed 's|.*refs/tags/||' | grep -E '^[0-9]+\.[0-9]+\.[0-9]+$' \
      | sort -t. -k1,1n -k2,2n -k3,3n | tail -n 1 || true)"
  fi
  echo "$ver"
}

if [ -f "$DEPS/Lilu.kext/Contents/Resources/Library/plugin_start.cpp" ] && [ -f "$DEPS/Lilu.kext/Contents/Resources/Headers/kern_api.hpp" ]; then
  echo "Lilu.kext already present, skipping."
else
  if [ -z "$LILU_VERSION" ]; then
    LILU_VERSION="$(latest_lilu_version)"
  fi
  if ! [[ "$LILU_VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "ERROR: could not determine the Lilu version (got '$LILU_VERSION')."
    echo "       Set it by hand, e.g.: LILU_VERSION=1.7.2 ./bootstrap.sh"
    exit 1
  fi

  echo "Fetching Lilu $LILU_VERSION (DEBUG release)..."
  tmp="$(mktemp -d)"
  trap 'rm -rf "$tmp"' EXIT
  url="https://github.com/$LILU_REPO/releases/download/$LILU_VERSION/Lilu-$LILU_VERSION-DEBUG.zip"
  curl -fsSL -o "$tmp/lilu.zip" "$url" || { echo "ERROR: download failed: $url"; exit 1; }
  unzip -q "$tmp/lilu.zip" -d "$tmp/out"

  [ -f "$tmp/out/Lilu.kext/Contents/Resources/Library/plugin_start.cpp" ] &&
  [ -f "$tmp/out/Lilu.kext/Contents/Resources/Headers/kern_api.hpp" ] || {
    echo "ERROR: the archive does not contain the Lilu SDK (Contents/Resources/{Headers,Library})."
    exit 1
  }
  rm -rf "$DEPS/Lilu.kext"
  mv "$tmp/out/Lilu.kext" "$DEPS/Lilu.kext"
fi

echo "Done. Next: make doctor && make"
