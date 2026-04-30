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

# ffmpeg - used for WAV → OGG transcoding on GP5 imports.
# verify_bundled_binaries treats resources/bin/ffmpeg as required and
# will hard-fail later if it's missing — fail here with the actual cause
# rather than letting that downstream check produce a less-direct error.
if command -v ffmpeg >/dev/null 2>&1; then
    cp "$(which ffmpeg)" "$BIN_DIR/"
    echo " ffmpeg: $(ls -lh "$BIN_DIR/ffmpeg" | awk '{print $5}')"
else
    echo "ERROR: ffmpeg not found on PATH; resources/bin/ffmpeg is required for the bundled build (apt: ffmpeg / brew: ffmpeg)." >&2
    exit 1
fi

# vgmstream-cli - used for Rocksmith WEM → WAV decoding.
# Download from GitHub releases if not in PATH (CI does this inline).
# verify_bundled_binaries downstream treats this as required and will
# hard-fail if it ends up missing — fail here with the actual cause
# instead.
if command -v vgmstream-cli >/dev/null 2>&1; then
    cp "$(which vgmstream-cli)" "$BIN_DIR/"
    echo " vgmstream-cli: $(ls -lh "$BIN_DIR/vgmstream-cli" | awk '{print $5}')"
else
    for tool in curl unzip; do
        if ! command -v "$tool" >/dev/null 2>&1; then
            echo "ERROR: $tool not on PATH; required to download/extract vgmstream-cli." >&2
            exit 1
        fi
    done

    echo "Downloading vgmstream-cli from GitHub releases..."
    VGM_ASSET="vgmstream-linux-cli.zip"
    if ! curl -sL --fail --retry 5 --retry-delay 5 --retry-all-errors \
            "https://github.com/vgmstream/vgmstream/releases/latest/download/${VGM_ASSET}" \
            -o /tmp/vgmstream.zip; then
        echo "ERROR: failed to download vgmstream-cli zip from upstream releases." >&2
        exit 1
    fi
    if ! unzip -q /tmp/vgmstream.zip -d /tmp/vgmstream; then
        echo "ERROR: failed to extract /tmp/vgmstream.zip — upstream archive may be malformed." >&2
        exit 1
    fi
    VGM_BIN=$(find /tmp/vgmstream -maxdepth 2 -name 'vgmstream-cli' -type f | head -1)
    if [ -z "$VGM_BIN" ]; then
        echo "ERROR: vgmstream-cli binary not found in downloaded archive — upstream zip layout may have changed." >&2
        exit 1
    fi
    cp "$VGM_BIN" "$BIN_DIR/vgmstream-cli"
    chmod +x "$BIN_DIR/vgmstream-cli"
    echo " vgmstream-cli: $(ls -lh "$BIN_DIR/vgmstream-cli" | awk '{print $5}') (downloaded)"
    rm -rf /tmp/vgmstream /tmp/vgmstream.zip
fi

# vgmstream's shared libs - its binary links against libvgmstream /
# libjansson / libmpg123 which aren't always on end-user systems.
for lib in libvgmstream libjansson libmpg123; do
    found=$(ldconfig -p 2>/dev/null | (grep "$lib" || true) | head -1 | awk '{print $NF}')
    if [ -n "$found" ] && [ -f "$found" ]; then
        cp -L "$found" "$BIN_DIR/"
    fi
done

# fluidsynth - used for MIDI → WAV in GP5 imports. Bundle the binary
# AND its non-glibc shared-library dependencies so the copy works on
# clean user machines. Set RPATH to $ORIGIN so the binary loads its
# siblings from its own directory at runtime.
if command -v fluidsynth >/dev/null 2>&1; then
    cp "$(which fluidsynth)" "$BIN_DIR/fluidsynth"
    echo " fluidsynth: $(ls -lh "$BIN_DIR/fluidsynth" | awk '{print $5}')"

    if ! command -v patchelf >/dev/null 2>&1; then
        echo " ERROR: patchelf not found - install it (apt: patchelf) so bundled fluidsynth can find its libs at runtime" >&2
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
    echo "ERROR: fluidsynth not found on PATH - it is required to bundle GP5 import support. Install fluidsynth and rerun this script (apt: fluidsynth)." >&2
    exit 1
fi

echo " Total resources/bin/: $(du -sh "$BIN_DIR" | cut -f1)"
echo "=== Binary bundle complete ==="
