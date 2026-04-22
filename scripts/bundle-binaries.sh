#!/bin/bash
# Bundle system binaries (ffmpeg + vgmstream-cli + fluidsynth) into
# resources/bin/, and for fluidsynth also copy its non-system shared
# libraries so the bundle runs on end-user systems without the
# libfluidsynth3/libasound/libglib/etc. packages installed.
#
# Linux-only. macOS bundling runs dylibbundler inline in the CI workflow
# because it's a different dynamic-linker story (Mach-O load paths +
# codesign invalidation). Windows downloads pinned zip archives inline.

set -euo pipefail

if [ "$(uname -s)" != "Linux" ]; then
    echo "bundle-binaries.sh is Linux-only. macOS/Windows bundling runs inline in CI." >&2
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BIN_DIR="$PROJECT_DIR/resources/bin"

mkdir -p "$BIN_DIR"
echo "=== Bundling system binaries ==="

# ffmpeg — used for WAV → OGG transcoding on GP5 imports.
if command -v ffmpeg >/dev/null 2>&1; then
    cp "$(which ffmpeg)" "$BIN_DIR/"
    echo "  ffmpeg: $(ls -lh "$BIN_DIR/ffmpeg" | awk '{print $5}')"
else
    echo "  WARNING: ffmpeg not found on PATH" >&2
fi

# vgmstream-cli — used for Rocksmith WEM → WAV decoding.
if command -v vgmstream-cli >/dev/null 2>&1; then
    cp "$(which vgmstream-cli)" "$BIN_DIR/"
    echo "  vgmstream-cli: $(ls -lh "$BIN_DIR/vgmstream-cli" | awk '{print $5}')"
else
    echo "  WARNING: vgmstream-cli not found on PATH (CI downloads the release zip)" >&2
fi

# vgmstream's shared libs — its binary links against libvgmstream /
# libjansson / libmpg123 which aren't always on end-user systems.
for lib in libvgmstream libjansson libmpg123; do
    found=$(ldconfig -p 2>/dev/null | grep "$lib" | head -1 | awk '{print $NF}')
    if [ -n "$found" ] && [ -f "$found" ]; then
        cp -L "$found" "$BIN_DIR/"
    fi
done

# fluidsynth — used for MIDI → WAV in GP5 imports. Bundle the binary
# AND its non-glibc shared-library dependencies so the copy works on
# clean user machines. Set RPATH to $ORIGIN so the binary loads its
# siblings from its own directory at runtime.
if command -v fluidsynth >/dev/null 2>&1; then
    cp "$(which fluidsynth)" "$BIN_DIR/fluidsynth"
    echo "  fluidsynth: $(ls -lh "$BIN_DIR/fluidsynth" | awk '{print $5}')"

    if ! command -v patchelf >/dev/null 2>&1; then
        echo "  ERROR: patchelf not found — install it (apt: patchelf) so bundled fluidsynth can find its libs at runtime" >&2
        exit 1
    fi

    ldd "$BIN_DIR/fluidsynth" | awk '/=>/ {print $3}' | while read -r lib; do
        [ -n "$lib" ] && [ -f "$lib" ] || continue
        case "$(basename "$lib")" in
            libc.so*|libm.so*|libpthread.so*|libdl.so*|librt.so*|\
            ld-linux*|libresolv.so*|linux-vdso*|linux-gate*|\
            libnsl.so*|libutil.so*|libgcc_s.so*)
                continue
                ;;
        esac
        cp -L "$lib" "$BIN_DIR/"
    done

    patchelf --set-rpath '$ORIGIN' "$BIN_DIR/fluidsynth"
    for so in "$BIN_DIR"/*.so*; do
        [ -f "$so" ] && patchelf --set-rpath '$ORIGIN' "$so" 2>/dev/null || true
    done
else
    echo "  WARNING: fluidsynth not found on PATH — GP5 import will fail at runtime without a system install" >&2
fi

echo "  Total resources/bin/: $(du -sh "$BIN_DIR" | cut -f1)"
echo "=== Binary bundle complete ==="
