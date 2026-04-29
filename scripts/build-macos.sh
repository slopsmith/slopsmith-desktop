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
bundle_python_impl() {
    # macOS: copy system Python installation
    PYTHON_PREFIX=$(python3 -c "import sys; print(sys.prefix)")
    mkdir -p "$PROJECT_DIR/resources/python"
    cp -R "$PYTHON_PREFIX" "$PROJECT_DIR/resources/python/runtime"

    # Install Python packages
    "$PROJECT_DIR/resources/python/runtime/bin/python3" -m pip install --quiet --no-cache-dir \
        $PYTHON_PACKAGES 2>&1 | tail -5
}

# Platform-specific: Return expected artifact patterns
get_expected_artifacts() {
    printf "%s\n" "$PROJECT_DIR/release/*.dmg" "$PROJECT_DIR/release/*.zip"
}

# Platform-specific: Bundle system binaries
bundle_binaries_impl() {
    # macOS: copy existing binaries and bundle dependencies

    # ffmpeg
    if command -v ffmpeg &>/dev/null; then
        cp "$(which ffmpeg)" "$PROJECT_DIR/resources/bin/"
    fi

    # fluidsynth
    if command -v fluidsynth &>/dev/null; then
        cp "$(which fluidsynth)" "$PROJECT_DIR/resources/bin/"
    fi

    # vgmstream (download release)
    curl -sL --fail --retry 5 --retry-delay 5 --retry-all-errors \
        "https://github.com/vgmstream/vgmstream/releases/latest/download/vgmstream-mac-cli.zip" \
        -o /tmp/vgmstream.zip
    unzip -q /tmp/vgmstream.zip -d /tmp/vgmstream
    VGM_BIN=$(find /tmp/vgmstream -name 'vgmstream-cli' -type f | head -1)
    if [[ -n "$VGM_BIN" ]]; then
        cp "$VGM_BIN" "$PROJECT_DIR/resources/bin/vgmstream-cli"
        chmod +x "$PROJECT_DIR/resources/bin/vgmstream-cli"
    fi

    # Use dylibbundler if available (for fluidsynth dependencies)
    if command -v dylibbundler &>/dev/null && [[ -f "$PROJECT_DIR/resources/bin/fluidsynth" ]]; then
        dylibbundler -cd -b -x "$PROJECT_DIR/resources/bin/fluidsynth" -d "$PROJECT_DIR/resources/bin" -p '@executable_path/'
    fi
}

# Run the build
main "$@"
