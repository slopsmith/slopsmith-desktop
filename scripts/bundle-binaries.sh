#!/bin/bash
# Bundle system binaries (ffmpeg, ffprobe, vgmstream-cli, fluidsynth)
# into resources/bin/ along with their non-glibc shared library
# dependencies, then set RPATH=$ORIGIN so each binary loads its
# siblings from its own directory at runtime.
#
# Without this, a build host with a different ffmpeg ABI than the user
# (e.g. Ubuntu 22.04 ffmpeg 4.x → libav*.so.58, Fedora 44 / Arch
# ffmpeg 7.x → libav*.so.62) ships a binary the user can't load.
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

# patchelf is required to set RPATH=$ORIGIN on bundled binaries and
# libs. Without that, the runtime linker on the user's machine falls
# back to /usr/lib and fails when the host ABI doesn't match the build
# host's. Fail here rather than letting fluidsynth's section fail later.
if ! command -v patchelf >/dev/null 2>&1; then
    echo "ERROR: patchelf not found on PATH (apt: patchelf) - required to set RPATH on bundled binaries." >&2
    exit 1
fi

# Library basenames we deliberately do NOT bundle: low-level libc /
# loader pieces that MUST come from the user's own glibc. Bundling
# these across distros breaks the dynamic linker.
is_skipped_lib() {
    case "$1" in
        libc.so*|libm.so*|libpthread.so*|libdl.so*|librt.so*|\
        ld-linux*|libresolv.so*|linux-vdso*|linux-gate*|\
        libnsl.so*|libutil.so*|libgcc_s.so*)
            return 0 ;;
    esac
    return 1
}

# Copy every non-glibc shared library that the given binary links to
# into resources/bin/. The final patchelf sweep (below) sets RPATH on
# every binary and every .so so the directory becomes a single
# self-contained load tree.
bundle_with_deps() {
    local bin_path="$1"
    # ldd exits non-zero (and prints "not a dynamic executable") on
    # statically linked binaries like the vgmstream-cli GitHub release.
    # Trap that and treat it as "no deps to bundle" rather than letting
    # pipefail kill the build.
    local ldd_out
    if ! ldd_out=$(ldd "$bin_path" 2>/dev/null); then
        return 0
    fi
    echo "$ldd_out" | awk '/=>/ {print $3}' | while read -r lib; do
        [ -n "$lib" ] && [ -f "$lib" ] || continue
        if is_skipped_lib "$(basename "$lib")"; then
            continue
        fi
        # -L follows symlinks; -n avoids re-copying a lib already
        # contributed by an earlier binary (every bundled binary on
        # the same build host links the same /usr/lib versions, so
        # first-wins is safe).
        cp -Ln "$lib" "$BIN_DIR/" 2>/dev/null || true
    done
}

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

# Sloppak conversion encodes .ogg with -c:a libvorbis. Fail the build now
# rather than ship an ffmpeg that produces "Unknown encoder 'libvorbis'"
# at runtime on user machines. The lib/sloppak_convert.py fallback to
# the built-in `vorbis -strict experimental` encoder is a safety net for
# unbundled installs, not a license to ship a libvorbis-less binary.
if ! "$BIN_DIR/ffmpeg" -hide_banner -encoders 2>/dev/null | grep -wq libvorbis; then
    echo "ERROR: bundled ffmpeg lacks libvorbis encoder. Sloppak conversion would fall back to the lower-quality built-in vorbis encoder on user machines." >&2
    echo "Install an ffmpeg built with --enable-libvorbis (apt's ffmpeg ships it by default; check your distro's package if this fails)." >&2
    exit 1
fi

bundle_with_deps "$BIN_DIR/ffmpeg"

# ffprobe - demucs's audio loader spawns ffprobe before ffmpeg to read
# stream metadata; falling through to a host-installed ffprobe (or none
# at all) means the desktop bundle behaves differently on each user's
# machine. Ship the build host's ffprobe alongside ffmpeg so the bundle
# is self-contained on every platform. apt's ffmpeg package includes
# ffprobe, so this is universally available where ffmpeg already is.
if command -v ffprobe >/dev/null 2>&1; then
    cp "$(which ffprobe)" "$BIN_DIR/"
    echo " ffprobe: $(ls -lh "$BIN_DIR/ffprobe" | awk '{print $5}')"
else
    echo "ERROR: ffprobe not found on PATH; resources/bin/ffprobe is required so demucs can read stream metadata in stem-splitting (apt: ffmpeg / brew: ffmpeg)." >&2
    exit 1
fi

bundle_with_deps "$BIN_DIR/ffprobe"

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

bundle_with_deps "$BIN_DIR/vgmstream-cli"

# fluidsynth - used for MIDI → WAV in GP5 imports.
if command -v fluidsynth >/dev/null 2>&1; then
    cp "$(which fluidsynth)" "$BIN_DIR/fluidsynth"
    echo " fluidsynth: $(ls -lh "$BIN_DIR/fluidsynth" | awk '{print $5}')"
else
    echo "ERROR: fluidsynth not found on PATH - it is required to bundle GP5 import support. Install fluidsynth and rerun this script (apt: fluidsynth)." >&2
    exit 1
fi

bundle_with_deps "$BIN_DIR/fluidsynth"

# Final patchelf sweep: every dynamic binary gets RPATH=$ORIGIN, and
# every dynamic .so does too so transitive deps also load from
# resources/bin/. Done once at the end so the order in which binaries
# contribute their libs doesn't matter.
#
# Be strict about patchelf success on dynamic binaries. The downstream
# audit only checks lib *presence*, not RPATH — so a silent patchelf
# failure here would ship a binary whose loader falls back to /usr/lib
# at runtime, exactly the issue #68 regression. Detect static-vs-dynamic
# explicitly via NEEDED entries in the .dynamic section and only skip
# patchelf on the static case (e.g. the vgmstream-cli GitHub release).
for bin in ffmpeg ffprobe vgmstream-cli fluidsynth; do
    bin_path="$BIN_DIR/$bin"
    if readelf -d "$bin_path" 2>/dev/null | grep -q '(NEEDED)'; then
        patchelf --set-rpath '$ORIGIN' "$bin_path"
    else
        echo " $bin: statically linked, RPATH not applicable"
    fi
done
for so in "$BIN_DIR"/*.so*; do
    [ -f "$so" ] || continue
    if readelf -d "$so" 2>/dev/null | grep -q '(NEEDED)'; then
        patchelf --set-rpath '$ORIGIN' "$so"
    fi
done

echo " Total resources/bin/: $(du -sh "$BIN_DIR" | cut -f1)"
echo "=== Binary bundle complete ==="
