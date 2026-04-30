#!/bin/bash
# Sign every native binary inside resources/bin and resources/python/runtime
# with the Developer ID Application certificate so the .app passes
# notarization. Runs as part of the macOS bundle step, after binaries
# are downloaded/copied but before verify_bundled_binaries runs them
# (signing also clears the quarantine attribute that GitHub release
# downloads carry by default).
#
# Skips silently when APPLE_SIGNING_IDENTITY is unset — local builds
# without a Developer ID cert still produce a (Gatekeeper-rejected)
# unsigned .app, same as before this script existed.
#
# Required env (set by CI):
#   APPLE_SIGNING_IDENTITY  Full identity string, e.g.
#                           "Developer ID Application: Name (TEAMID)"
#                           Must already be present in the active keychain.

set -euo pipefail

if [[ -z "${APPLE_SIGNING_IDENTITY:-}" ]]; then
    echo "[sign-macos] APPLE_SIGNING_IDENTITY not set — skipping (unsigned build)"
    exit 0
fi

if [[ "$OSTYPE" != "darwin"* ]]; then
    echo "[sign-macos] not on macOS — skipping" >&2
    exit 0
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
ENTITLEMENTS="$PROJECT_DIR/resources/entitlements.mac.plist"

if [[ ! -f "$ENTITLEMENTS" ]]; then
    echo "[sign-macos] entitlements file missing: $ENTITLEMENTS" >&2
    exit 1
fi

sign_one() {
    local target="$1"
    [[ -e "$target" ]] || return 0
    # Skip symlinks — they get followed during bundling and signing the
    # link target separately is enough.
    [[ -L "$target" ]] && return 0
    echo "  $target"
    codesign --force --options runtime --timestamp \
        --sign "$APPLE_SIGNING_IDENTITY" \
        --entitlements "$ENTITLEMENTS" \
        "$target"
}

echo "[sign-macos] signing bundled binaries with: $APPLE_SIGNING_IDENTITY"

# 1. Top-level executables and bundled dylibs in resources/bin.
#    dylibbundler copies fluidsynth's deps into this directory and
#    rewrites their install names to @executable_path/, so each one
#    has to be signed individually before the app bundle is built.
BIN_DIR="$PROJECT_DIR/resources/bin"
if [[ -d "$BIN_DIR" ]]; then
    while IFS= read -r -d '' f; do
        sign_one "$f"
    done < <(find "$BIN_DIR" -maxdepth 1 -type f \( -perm -u+x -o -name '*.dylib' \) -print0)
fi

# 2. Embedded CPython runtime — interpreter + libpython dylib + every
#    compiled extension. Notarization rejects the bundle if any of
#    these is unsigned.
PY_RUNTIME="$PROJECT_DIR/resources/python/runtime"
if [[ -d "$PY_RUNTIME" ]]; then
    # Interpreter binaries (python3, python3.x, etc.)
    while IFS= read -r -d '' f; do
        sign_one "$f"
    done < <(find "$PY_RUNTIME/bin" -maxdepth 1 -type f -perm -u+x -print0 2>/dev/null || true)

    # libpython dylib(s) and any other shared libs
    while IFS= read -r -d '' f; do
        sign_one "$f"
    done < <(find "$PY_RUNTIME/lib" -maxdepth 2 -type f -name '*.dylib' -print0 2>/dev/null || true)

    # Compiled extension modules — both stdlib lib-dynload and any
    # site-packages C extensions installed via pip.
    while IFS= read -r -d '' f; do
        sign_one "$f"
    done < <(find "$PY_RUNTIME/lib" -type f \( -name '*.so' -o -name '*.dylib' \) -print0 2>/dev/null || true)
fi

# 3. RsCli (.NET self-contained publish) lives under resources/rscli/
#    and ships its own native libs + executable. Sign whatever's there.
RSCLI_DIR="$PROJECT_DIR/resources/rscli"
if [[ -d "$RSCLI_DIR" ]]; then
    while IFS= read -r -d '' f; do
        sign_one "$f"
    done < <(find "$RSCLI_DIR" -type f \( -perm -u+x -o -name '*.dylib' -o -name '*.so' \) -print0)
fi

echo "[sign-macos] done"

# Spot-check a couple of binaries so a botched sign call fails the
# build here rather than mid-notarization 2 minutes later.
for probe in "$BIN_DIR/fluidsynth" "$BIN_DIR/ffmpeg" "$BIN_DIR/vgmstream-cli"; do
    if [[ -f "$probe" ]]; then
        codesign --verify --strict "$probe" || {
            echo "[sign-macos] verification failed: $probe" >&2
            exit 1
        }
    fi
done
