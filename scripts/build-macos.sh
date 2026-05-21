#!/bin/bash
# Native macOS build script
# Uses Homebrew for dependencies and system Python

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
CONFIG="$PROJECT_DIR/.build-config.json"

# Platform identifier
export PLATFORM="macos"

# Check we're on macOS
if [[ "$OSTYPE" != "darwin"* ]]; then
    echo "Error: This script is for macOS only" >&2
    exit 1
fi

echo "=== Slopsmith Desktop macOS Build ==="
echo ""

# Disable electron-builder's keychain-identity auto-discovery on unsigned
# builds. Without this, electron-builder picks the first codesigning
# identity it finds (often an "Apple Development" cert from Xcode) and
# tries to sign with it — which both produces unusable artifacts AND
# fails when Slopsmith.app contains paths the Apple Development cert
# can't sign. Signed CI builds set APPLE_SIGNING_IDENTITY / CSC_NAME, so
# this guard only triggers for local unsigned dev builds.
if [[ -z "${APPLE_SIGNING_IDENTITY:-}" && -z "${CSC_NAME:-}" && -z "${CSC_LINK:-}" ]]; then
    export CSC_IDENTITY_AUTO_DISCOVERY=false
fi

# Derive CSC_NAME (electron-builder's identity name) from
# APPLE_SIGNING_IDENTITY. codesign accepts the full identity string with
# "Developer ID Application:" prefix; electron-builder rejects that
# prefix and wants the bare team-name + team-id form. Strip the prefix
# once here so the rest of the build (sign-macos-binaries.sh and
# electron-builder) can each consume the form they expect.
if [[ -z "${CSC_NAME:-}" && -n "${APPLE_SIGNING_IDENTITY:-}" ]]; then
    export CSC_NAME="${APPLE_SIGNING_IDENTITY#Developer ID Application: }"
fi

# Color setup
export RED='\033[0;31m'
export GREEN='\033[0;32m'
export YELLOW='\033[1;33m'
export BLUE='\033[0;34m'
export NC='\033[0m'

# Source common build logic
source "$SCRIPT_DIR/build-common.sh"

# Platform-specific: Install system dependencies
install_system_deps() {
    if command -v brew &>/dev/null; then
        PACKAGES=$(grep -v '^[[:space:]]*#' "$PROJECT_DIR/.packages/brew.txt" | grep -v '^[[:space:]]*$' | tr '\n' ' ')
        if [[ -n "$PACKAGES" ]]; then
            brew install $PACKAGES
        fi
    else
        echo "Error: Homebrew not found. Install from https://brew.sh" >&2
        exit 1
    fi
}

# Platform-specific: Bundle Python runtime
#
# Uses python-build-standalone (Astral) — a fully relocatable CPython
# distribution built specifically for redistribution. Avoids every
# hazard of trying to copy a Homebrew framework: no PEP 668 marker, no
# install_name_tool dance, no broken site-packages symlink, sys.prefix
# correctly resolves to the bundle's location at runtime.
bundle_python_impl() {
    local config_py
    config_py=$(python3 "$SCRIPT_DIR/parse-build-config.py" "$PROJECT_DIR/.build-config.json" .versions.python)
    local py_mm="${config_py%.*}"

    local arch
    arch=$(uname -m)
    local config_key
    case "$arch" in
        arm64|aarch64) config_key="python_standalone_macos_arm64" ;;
        x86_64)        config_key="python_standalone_macos_x64" ;;
        *)
            echo "Error: unsupported macOS arch: $arch" >&2
            exit 1
            ;;
    esac

    local pbs_url pbs_sha
    pbs_url=$(python3 "$SCRIPT_DIR/parse-build-config.py" "$PROJECT_DIR/.build-config.json" ".external.${config_key}.url")
    pbs_sha=$(python3 "$SCRIPT_DIR/parse-build-config.py" "$PROJECT_DIR/.build-config.json" ".external.${config_key}.sha256")

    local runtime="$PROJECT_DIR/resources/python/runtime"
    local tarball="/tmp/cpython-${config_py}-macos-${arch}.tar.gz"

    mkdir -p "$PROJECT_DIR/resources/python"
    rm -rf "$runtime"

    if [[ ! -f "$tarball" ]] || ! shasum -a 256 "$tarball" | awk '{print $1}' | grep -qx "$pbs_sha"; then
        echo "  Downloading python-build-standalone ${config_py} (${arch})"
        curl -sL --fail --retry 5 --retry-delay 5 --retry-all-errors "$pbs_url" -o "$tarball"
    fi
    local actual_sha
    actual_sha=$(shasum -a 256 "$tarball" | awk '{print $1}')
    if [[ "$actual_sha" != "$pbs_sha" ]]; then
        echo "Error: python-build-standalone tarball SHA256 mismatch" >&2
        echo "  expected: $pbs_sha" >&2
        echo "  got:      $actual_sha" >&2
        exit 1
    fi

    # PBS tarballs extract to a top-level `python/` dir; rename to
    # `runtime` so the rest of the build (and python.ts) finds the
    # interpreter at resources/python/runtime/bin/python3.
    local extract_dir="/tmp/pbs-extract-$$"
    rm -rf "$extract_dir"
    mkdir -p "$extract_dir"
    tar -xzf "$tarball" -C "$extract_dir"
    mv "$extract_dir/python" "$runtime"
    rm -rf "$extract_dir"

    # PBS tarballs ship a working pip pre-installed in the bundle's
    # site-packages, and `bin/python3` is a real binary (not a symlink),
    # so the install is fully relocatable as-is.
    #
    # Install slopsmith's runtime requirements first (single source of
    # truth — drift used to silently break desktop builds whenever
    # slopsmith added a dep), then desktop-only extras. SLOPSMITH_DIR
    # is exported by clone_slopsmith() in build-common.sh; fall back
    # to local-dev paths to match bundle-slopsmith.sh's discovery so
    # this script works outside CI too.
    if [[ -z "${SLOPSMITH_DIR:-}" ]]; then
        if [[ -d "$PROJECT_DIR/../slopsmith" ]]; then
            SLOPSMITH_DIR="$PROJECT_DIR/../slopsmith"
        elif [[ -d "$HOME/Repositories/slopsmith" ]]; then
            SLOPSMITH_DIR="$HOME/Repositories/slopsmith"
        fi
    fi
    if [[ -z "${SLOPSMITH_DIR:-}" ]] || [[ ! -f "$SLOPSMITH_DIR/requirements.txt" ]]; then
        echo "ERROR: slopsmith requirements.txt not found (SLOPSMITH_DIR=${SLOPSMITH_DIR:-<unset>})." >&2
        echo "       Expected SLOPSMITH_DIR to be exported by clone_slopsmith() in build-common.sh," >&2
        echo "       or slopsmith cloned next to this repo." >&2
        exit 1
    fi
    "$runtime/bin/python3" -m pip install --quiet --no-cache-dir \
        -r "$SLOPSMITH_DIR/requirements.txt" 2>&1 | tail -5
    "$runtime/bin/python3" -m pip install --quiet --no-cache-dir \
        -r "$PROJECT_DIR/.packages/python.txt" 2>&1 | tail -5
}

# Platform-specific: Return expected artifact patterns
get_expected_artifacts() {
    # mac.target is "dir": electron-builder writes the unpacked
    # Slopsmith.app to release/mac-arm64/ (no .dmg/.zip). Velopack's
    # pack step turns that .app into the actual release assets.
    printf "%s\n" "$PROJECT_DIR/release/mac-arm64/Slopsmith.app"
}

# Platform-specific: Bundle system binaries
bundle_binaries_impl() {
    # macOS: copy existing binaries and bundle dependencies

    # ffmpeg + ffprobe (static builds, NOT brew's ffmpeg).
    #
    # Homebrew's stock `ffmpeg` formula (8.1.1+) no longer ships
    # --enable-libvorbis. Sloppak conversion encodes .ogg with
    # `-c:a libvorbis`, so a brew-ffmpeg-bundled desktop app silently
    # degrades to the built-in `vorbis -strict experimental` encoder on
    # user machines. Pull the matching arch's static build instead:
    #   - osxexperts.net for arm64 (Apple Silicon)
    #   - evermeet.cx   for x86_64 (Intel)
    # Both ship with --enable-libvorbis; URLs + SHA256 pins live in
    # .build-config.json so an upstream rebuild surfaces as a SHA
    # mismatch rather than a silent codec change. demucs needs ffprobe
    # alongside ffmpeg (it reads stream metadata before invoking the
    # encoder), so we download both from the same provider.
    local arch ff_key fp_key
    arch=$(uname -m)
    case "$arch" in
        arm64|aarch64) ff_key="ffmpeg_macos_arm64";  fp_key="ffprobe_macos_arm64" ;;
        x86_64)        ff_key="ffmpeg_macos_x64";    fp_key="ffprobe_macos_x64"   ;;
        *)
            echo "Error: unsupported macOS arch: $arch" >&2
            exit 1
            ;;
    esac

    local bin_dir="$PROJECT_DIR/resources/bin"
    mkdir -p "$bin_dir"

    download_and_install_macos_ffmpeg_tool() {
        # $1 = tool name (ffmpeg|ffprobe), $2 = config key
        local tool="$1" key="$2"
        local url sha tarball
        url=$(python3 "$SCRIPT_DIR/parse-build-config.py" "$CONFIG" ".external.${key}.url")
        sha=$(python3 "$SCRIPT_DIR/parse-build-config.py" "$CONFIG" ".external.${key}.sha256")
        tarball="/tmp/${tool}-macos-${arch}.zip"

        if [[ ! -f "$tarball" ]] || ! shasum -a 256 "$tarball" | awk '{print $1}' | grep -qx "$sha"; then
            echo "  Downloading $tool from $url"
            curl -sL --fail --retry 5 --retry-delay 5 --retry-all-errors "$url" -o "$tarball"
        fi
        local actual_sha
        actual_sha=$(shasum -a 256 "$tarball" | awk '{print $1}')
        if [[ "$actual_sha" != "$sha" ]]; then
            echo "Error: $tool zip SHA256 mismatch — upstream rebuilt under the same URL" >&2
            echo "  expected: $sha" >&2
            echo "  got:      $actual_sha" >&2
            echo "  url:      $url" >&2
            echo "Update .external.${key}.sha256 in .build-config.json after verifying the new binary." >&2
            exit 1
        fi

        local extract_dir="/tmp/${tool}-extract-$$"
        rm -rf "$extract_dir"
        mkdir -p "$extract_dir"
        # `find` after unzip tolerates either layout (osxexperts puts
        # the binary at the zip root, evermeet does too at the moment,
        # but the spec doesn't promise it). The osxexperts zip also
        # ships __MACOSX/._* resource forks alongside the binary; the
        # path filter avoids picking those up as the result.
        unzip -q -o "$tarball" -d "$extract_dir"
        local found
        found=$(find "$extract_dir" -type f -name "$tool" -not -path '*/__MACOSX/*' | head -1)
        if [[ -z "$found" ]]; then
            echo "Error: '$tool' binary not found after unzipping $tarball — upstream layout may have changed." >&2
            exit 1
        fi
        cp "$found" "$bin_dir/$tool"
        chmod +x "$bin_dir/$tool"
        # Static builds from third-party sites carry the macOS quarantine
        # xattr by default; clear it so the binary can be exec'd by the
        # build steps that follow (sign-macos-binaries.sh would also
        # strip this, but verify_bundled_binaries runs the binary first).
        xattr -d com.apple.quarantine "$bin_dir/$tool" 2>/dev/null || true
        rm -rf "$extract_dir"
    }

    download_and_install_macos_ffmpeg_tool ffmpeg  "$ff_key"
    download_and_install_macos_ffmpeg_tool ffprobe "$fp_key"

    # Sloppak conversion encodes .ogg with -c:a libvorbis. Verify the
    # downloaded ffmpeg actually has the encoder — both osxexperts and
    # evermeet build with --enable-libvorbis today, but pinning by SHA
    # already catches binary drift; this is the runtime guarantee. The
    # lib/sloppak_convert.py fallback is a safety net for unbundled
    # installs, not a license to ship a libvorbis-less binary.
    if ! "$bin_dir/ffmpeg" -hide_banner -encoders 2>/dev/null | grep -wq libvorbis; then
        echo "Error: bundled ffmpeg lacks libvorbis encoder. Sloppak conversion would fall back to the lower-quality built-in vorbis encoder on user machines." >&2
        echo "The pinned static build for arch=$arch ($ff_key) should include --enable-libvorbis; if it doesn't, pick a different upstream and update .build-config.json." >&2
        exit 1
    fi

    local fluidsynth_bin
    fluidsynth_bin="$(command -v fluidsynth || true)"
    if [[ -z "$fluidsynth_bin" ]]; then
        echo "Error: fluidsynth not found on PATH. Install it with \`brew install fluid-synth\` (and ensure /opt/homebrew/bin is on PATH)." >&2
        exit 1
    fi
    cp "$fluidsynth_bin" "$PROJECT_DIR/resources/bin/"

    # vgmstream (download release)
    echo -e "${BLUE}=== Downloading vgmstream-cli ===${NC}"
    curl -sL --fail --retry 5 --retry-delay 5 --retry-all-errors \
        "https://github.com/vgmstream/vgmstream/releases/latest/download/vgmstream-mac-cli.zip" \
        -o /tmp/vgmstream.zip
    echo "Downloaded vgmstream-mac-cli.zip"
    unzip -q /tmp/vgmstream.zip -d /tmp/vgmstream
    echo "Extracted to /tmp/vgmstream"
    echo "Contents of /tmp/vgmstream:"
    ls -la /tmp/vgmstream/
    VGM_BIN=$(find /tmp/vgmstream -name 'vgmstream-cli' -type f | head -1)
    echo "Found vgmstream-cli at: $VGM_BIN"
    if [[ -n "$VGM_BIN" ]]; then
        echo "Copying vgmstream-cli to resources/bin/"
        cp "$VGM_BIN" "$PROJECT_DIR/resources/bin/vgmstream-cli"
        chmod +x "$PROJECT_DIR/resources/bin/vgmstream-cli"
        echo "Copied binary details:"
        ls -la "$PROJECT_DIR/resources/bin/vgmstream-cli"
        file "$PROJECT_DIR/resources/bin/vgmstream-cli"
        # Remove macOS quarantine attribute (downloaded binaries are quarantined by default)
        xattr -d com.apple.quarantine "$PROJECT_DIR/resources/bin/vgmstream-cli" 2>/dev/null || true
        echo "Quarantine attributes after removal:"
        xattr -l "$PROJECT_DIR/resources/bin/vgmstream-cli" 2>/dev/null || echo "  (none)"
        # Test running the binary
        echo -e "${BLUE}=== Testing vgmstream-cli execution ===${NC}"
        echo "Running: $PROJECT_DIR/resources/bin/vgmstream-cli --help"
        "$PROJECT_DIR/resources/bin/vgmstream-cli" --help || echo -e "${RED}Failed to run vgmstream-cli${NC}"
        # Check dynamic dependencies
        echo -e "${BLUE}=== Checking dynamic dependencies ===${NC}"
        echo "Running: otool -L $PROJECT_DIR/resources/bin/vgmstream-cli"
        otool -L "$PROJECT_DIR/resources/bin/vgmstream-cli"
        echo -e "${GREEN}vgmstream-cli setup complete${NC}"
    else
        echo -e "${RED}ERROR: vgmstream-cli binary not found in extracted archive${NC}" >&2
        echo "Searching for any vgmstream binaries:" >&2
        find /tmp/vgmstream -type f -name '*vgmstream*' 2>/dev/null || echo "  (none found)" >&2
        # Bail now rather than continue to dylibbundler / signing /
        # verify_bundled_binaries on a partial bundle — the failure
        # would only surface much later with a less-direct error.
        exit 1
    fi

    # Run dylibbundler on every bundled binary so each one's brew deps
    # (libfluidsynth, libspeex, libmpg123, libvorbis, libogg, ffmpeg
    # libs, etc.) get copied into resources/bin/ and the binaries' load
    # commands get rewritten to @executable_path/. Without this,
    # vgmstream-cli (downloaded from upstream) at runtime asks dyld for
    # /opt/homebrew/opt/speex/lib/libspeex.1.dylib — fine on the dev
    # machine, fatal on every other Mac. ffmpeg has the same problem
    # against its own brew deps. dylibbundler is idempotent and skips
    # paths it has already rewritten, so the per-binary loop is safe
    # even when binaries share dylibs.
    if command -v dylibbundler &>/dev/null; then
        for bin in fluidsynth ffmpeg ffprobe vgmstream-cli; do
            local target="$PROJECT_DIR/resources/bin/$bin"
            [[ -f "$target" ]] || continue
            echo -e "${BLUE}Bundling ${bin} dependencies...${NC}"
            dylibbundler -cd -b -of -x "$target" \
                -d "$PROJECT_DIR/resources/bin" \
                -p '@executable_path/'
        done
    fi

    # Sign all bundled native binaries with the Developer ID Application
    # cert before verify_bundled_binaries runs them. Signing also clears
    # the macOS quarantine attribute that downloaded binaries carry, so
    # the verify step doesn't have to special-case quarantine. No-op
    # when APPLE_SIGNING_IDENTITY is unset (local dev without a cert).
    "$SCRIPT_DIR/sign-macos-binaries.sh"
}

# Run the build
main "$@"

# Post-build: notarize and staple the DMG. electron-builder notarizes
# and staples the .app, then builds + signs the DMG — but the DMG
# itself is not submitted to Apple's notary service, so it ships
# unstapled. That's fine for online installs (Gatekeeper checks the
# .app inside on first launch), but offline first launches and some
# enterprise tools want a stapled DMG. notarytool with --wait blocks
# until Apple finishes (usually 30s–3min), then stapler embeds the
# ticket so the DMG verifies offline. No-op when signing was off.
if [[ -n "${APPLE_SIGNING_IDENTITY:-}" && -n "${APPLE_ID:-}" \
        && -n "${APPLE_APP_SPECIFIC_PASSWORD:-}" \
        && -n "${APPLE_TEAM_ID:-}" ]]; then
    shopt -s nullglob
    for dmg in "$PROJECT_DIR"/release/*.dmg; do
        echo -e "${BLUE}Notarizing $(basename "$dmg") (wait for Apple)...${NC}"
        xcrun notarytool submit "$dmg" \
            --apple-id "$APPLE_ID" \
            --password "$APPLE_APP_SPECIFIC_PASSWORD" \
            --team-id "$APPLE_TEAM_ID" \
            --wait
        echo -e "${BLUE}Stapling notarization ticket to $(basename "$dmg")...${NC}"
        # `notarytool submit --wait` returns when Apple's notary service
        # accepts the submission, but the ticket can take an extra
        # 30-60 s to propagate to CloudKit (where `stapler` reads from).
        # Stapling immediately fails with `Error 65: Record not found`
        # on CI roughly half the time. Retry with backoff.
        staple_ok=0
        for attempt in 1 2 3 4 5; do
            if xcrun stapler staple "$dmg"; then
                staple_ok=1
                break
            fi
            echo "  staple attempt $attempt failed; waiting before retry..."
            sleep $((attempt * 15))
        done
        if [[ "$staple_ok" -ne 1 ]]; then
            echo -e "${RED}Failed to staple $(basename "$dmg") after 5 attempts${NC}" >&2
            exit 1
        fi
        xcrun stapler validate "$dmg"
    done
    shopt -u nullglob
fi
