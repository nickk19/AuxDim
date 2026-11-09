#!/bin/bash

#
#  bootstrap.sh
#  AuxDim
#
#  Fetches the build dependencies into Deps/. Needs only git, curl, unzip and shasum (or
#  sha256sum), all part of macOS with the Xcode Command Line Tools (or any Linux distribution).
#
#    Deps/MacKernelSDK/   kernel headers and libkmod.a        (acidanthera/MacKernelSDK, pinned commit)
#    Deps/Lilu.kext/      Lilu plugin SDK: headers + library  (acidanthera/Lilu, DEBUG build of a release)
#
#  Both are pinned to the versions the plugin is built and tested with, and the Lilu
#  archive is checked against its SHA-256.
#
#  Usage:
#    ./bootstrap.sh                       fetch what is missing (the pinned versions)
#    LILU_VERSION=1.7.1 ./bootstrap.sh    another Lilu release (use the one you boot with); not checksummed
#    SDK_COMMIT=<sha> ./bootstrap.sh      another MacKernelSDK commit
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

# The pinned versions. Change them together with a test build.
SDK_PINNED="3f750085caa17ec3a7880f11c11bf4f48cd6a164"
LILU_PINNED="1.7.2"
LILU_PINNED_SHA256="e95df95d82e8b151047abf359ecd9c853755c59293e19dfcc3907da8fbd8cf89" # Lilu-1.7.2-DEBUG.zip

SDK_COMMIT="${SDK_COMMIT:-$SDK_PINNED}"
LILU_VERSION="${LILU_VERSION:-$LILU_PINNED}"

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

sha256_of() {
  if command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$1" | awk '{print $1}'
  elif command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | awk '{print $1}'
  else
    echo ""
  fi
}

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
  have="$(git -C "$DEPS/MacKernelSDK" rev-parse HEAD 2>/dev/null || echo unknown)"
  echo "MacKernelSDK already present ($have), skipping."
  [ "$have" = "$SDK_COMMIT" ] || echo "NOTE: wanted $SDK_COMMIT; ./bootstrap.sh --force fetches it."
else
  echo "Fetching MacKernelSDK $SDK_COMMIT..."
  rm -rf "$DEPS/MacKernelSDK"
  git init -q "$DEPS/MacKernelSDK"
  git -C "$DEPS/MacKernelSDK" remote add origin "$SDK_REPO"
  # GitHub serves a single commit by its hash, so a shallow fetch is enough.
  git -C "$DEPS/MacKernelSDK" fetch -q --depth 1 origin "$SDK_COMMIT" ||
    { echo "ERROR: could not fetch MacKernelSDK commit $SDK_COMMIT"; exit 1; }
  git -C "$DEPS/MacKernelSDK" -c advice.detachedHead=false checkout -q FETCH_HEAD
  [ -f "$DEPS/MacKernelSDK/Library/x86_64/libkmod.a" ] || { echo "ERROR: MacKernelSDK has no Library/x86_64/libkmod.a"; exit 1; }
fi

# ---- Lilu ---------------------------------------------------------------------

if [ -f "$DEPS/Lilu.kext/Contents/Resources/Library/plugin_start.cpp" ] && [ -f "$DEPS/Lilu.kext/Contents/Resources/Headers/kern_api.hpp" ]; then
  have="$(sed -n '/<key>CFBundleVersion<\/key>/{n;s/.*<string>\(.*\)<\/string>.*/\1/p;}' "$DEPS/Lilu.kext/Contents/Info.plist" 2>/dev/null | head -n 1)"
  echo "Lilu.kext already present (${have:-unknown version}), skipping."
  [ "$have" = "$LILU_VERSION" ] || echo "NOTE: wanted Lilu $LILU_VERSION; ./bootstrap.sh --force fetches it."
else
  if ! [[ "$LILU_VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "ERROR: LILU_VERSION must be a release number like $LILU_PINNED (got '$LILU_VERSION')."
    exit 1
  fi

  echo "Fetching Lilu $LILU_VERSION (DEBUG release)..."
  tmp="$(mktemp -d)"
  trap 'rm -rf "$tmp"' EXIT
  url="https://github.com/$LILU_REPO/releases/download/$LILU_VERSION/Lilu-$LILU_VERSION-DEBUG.zip"
  curl -fsSL -o "$tmp/lilu.zip" "$url" || { echo "ERROR: download failed: $url"; exit 1; }
  sum="$(sha256_of "$tmp/lilu.zip")"
  if [ "$LILU_VERSION" = "$LILU_PINNED" ]; then
    [ "$sum" = "$LILU_PINNED_SHA256" ] || {
      echo "ERROR: Lilu-$LILU_VERSION-DEBUG.zip has SHA-256 '$sum', expected $LILU_PINNED_SHA256."
      exit 1
    }
    echo "SHA-256 verified."
  else
    echo "WARNING: no known checksum for Lilu $LILU_VERSION, the archive is not verified (SHA-256 ${sum:-unavailable})."
  fi
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
