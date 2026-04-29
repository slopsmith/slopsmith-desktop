#!/bin/bash

# Native Windows build script
# Runs in Git Bash (Git for Windows)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -W 2>/dev/null || pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
CONFIG="$PROJECT_DIR/.build-config.json"

# Platform identifier
export PLATFORM="windows"

# Check for Git Bash/MSYS
if [[ "$OSTYPE" != "msys" ]] && [[ "$OSTYPE" != "win32" ]] && [[ -z "${MSYSTEM:-}" ]]; then
    echo "Error: This script must be run in Git Bash (Git for Windows)" >&2
    echo "Download: https://git-scm.com/download/win" >&2
    exit 1
fi

echo "=== Slopsmith Desktop Windows Build ==="
echo ""

# Color setup
export RED='\033[0;31m'
export GREEN='\033[0;32m'
export YELLOW='\033[1;33m'
export BLUE='\033[0;34m'
export NC='\033[0m'

# Source common build logic
source "$SCRIPT_DIR/build-common.sh"

# Platform-specific: Return expected artifact patterns
get_expected_artifacts() {
    printf "%s\n" "$PROJECT_DIR/release/*.exe" "$PROJECT_DIR/release/*.zip"
}

# Platform-specific: Install system dependencies
install_system_deps() {
    # Windows: install via Chocolatey if available
    if command -v choco.exe &>/dev/null || command -v choco &>/dev/null; then
        choco install cmake ffmpeg --installargs 'ADD_CMAKE_TO_PATH=System' || echo "Chocolatey install may have failed, continuing..."
    else
        echo_warning "Chocolatey not found - skipping system package installation"
        echo "  Make sure cmake and ffmpeg are already in PATH"
    fi
}

# Platform-specific: Bundle Python runtime
bundle_python_impl() {
    # Windows: download embeddable Python
    PYTHON_VERSION=$(python3 "$SCRIPT_DIR/parse-build-config.py" "$CONFIG" .versions.python)
    PYTHON_MAJOR="${PYTHON_VERSION%%.*}"
    PYTHON_MINOR="${PYTHON_VERSION#*.}"
    PYTHON_EMBED_URL="https://www.python.org/ftp/python/${PYTHON_VERSION}/python-${PYTHON_VERSION}-embed-amd64.zip"
    
    echo "Downloading Python embeddable..."
    if ! curl -sL --fail --retry 5 --retry-delay 5 --retry-all-errors "$PYTHON_EMBED_URL" -o /tmp/python-embed.zip; then
        echo_error "Failed to download Python embeddable package"
        echo "  URL: $PYTHON_EMBED_URL"
        echo "  curl exit code: $?"
        exit 1
    fi
    
    mkdir -p "$PROJECT_DIR/resources/python"
    unzip -q /tmp/python-embed.zip -d "$PROJECT_DIR/resources/python/"
    
    # Enable site-packages by editing the ._pth file
    PTH_FILE=$(find "$PROJECT_DIR/resources/python" -name "*._pth" | head -1)
    if [[ -n "$PTH_FILE" ]]; then
        sed -i 's/#import site/import site/' "$PTH_FILE"
        echo "Lib/site-packages" >> "$PTH_FILE"
    fi
    
    # Install pip
    echo "Downloading pip..."
    if ! curl -sL --fail --retry 5 --retry-delay 5 --retry-all-errors \
        https://bootstrap.pypa.io/get-pip.py -o /tmp/get-pip.py; then
        echo_error "Failed to download pip installer"
        exit 1
    fi
    "$PROJECT_DIR/resources/python/python.exe" /tmp/get-pip.py --quiet --no-cache-dir

# Install packages
# Install build tools first (required for building from source on Windows embeddable Python)
"$PROJECT_DIR/resources/python/python.exe" -m pip install --quiet --no-cache-dir \
        setuptools wheel
# Install application packages
"$PROJECT_DIR/resources/python/python.exe" -m pip install --quiet --no-cache-dir \
        -r "$PROJECT_DIR/.packages/python.txt"
}

# Usage: download_with_retries <url> <output_path> <description>
download_with_retries() {
    local url="$1"
    local output_path="$2"
    local description="$3"
    local max_attempts=3
    local attempt=1
    local delay=10
    
    while [[ $attempt -le $max_attempts ]]; do
        echo "  Downloading $description (attempt $attempt/$max_attempts)..."
        if curl -sL --fail --max-time 120 "$url" -o "$output_path"; then
            echo "    Successfully downloaded $description"
            return 0
        fi
        
        local exit_code=$?
        echo "    Download failed with exit code $exit_code"
        
        if [[ $attempt -lt $max_attempts ]]; then
            echo "    Retrying in ${delay}s..."
            sleep $delay
            delay=$((delay * 2))
        fi
        
        attempt=$((attempt + 1))
    done
    
    echo_error "Failed to download $description after $max_attempts attempts"
    return 1
}

# Platform-specific: Bundle system binaries
# These binaries are REQUIRED for core functionality. The build will fail
# if downloads don't succeed after multiple retry attempts.
bundle_binaries_impl() {
    mkdir -p "$PROJECT_DIR/resources/bin"
    
    # ffmpeg static build
    echo "Downloading ffmpeg..."
    if ! download_with_retries \
        "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-win64-gpl.zip" \
        "/tmp/ffmpeg.zip" \
        "ffmpeg"; then
        exit 1
    fi
    unzip -q /tmp/ffmpeg.zip -d /tmp/ffmpeg
    cp /tmp/ffmpeg/ffmpeg-master-latest-win64-gpl/bin/ffmpeg.exe "$PROJECT_DIR/resources/bin/" 2>/dev/null || true
    
    # vgmstream-cli
    echo "Downloading vgmstream-cli..."
    if ! download_with_retries \
        "https://github.com/vgmstream/vgmstream/releases/latest/download/vgmstream-win64.zip" \
        "/tmp/vgmstream.zip" \
        "vgmstream-cli"; then
        exit 1
    fi
    unzip -q /tmp/vgmstream.zip -d /tmp/vgmstream
    cp /tmp/vgmstream/vgmstream-cli.exe "$PROJECT_DIR/resources/bin/" 2>/dev/null || true
    cp /tmp/vgmstream/*.dll "$PROJECT_DIR/resources/bin/" 2>/dev/null || true
    
    # fluidsynth
    echo "Downloading fluidsynth..."
    FS_URL=$(python3 -c "import json; print(json.load(open('$CONFIG')).get('external', {}).get('fluidsynth_windows', {}).get('url', ''))" 2>/dev/null)
    if [[ -n "$FS_URL" ]]; then
        if ! download_with_retries \
            "$FS_URL" \
            "/tmp/fluidsynth.zip" \
            "fluidsynth"; then
            exit 1
        fi
        unzip -q /tmp/fluidsynth.zip -d /tmp/fluidsynth
        FS_BIN=$(find /tmp/fluidsynth -name 'fluidsynth.exe' -type f | head -1)
        if [[ -n "$FS_BIN" ]]; then
            cp "$FS_BIN" "$PROJECT_DIR/resources/bin/"
            cp "$(dirname "$FS_BIN")"/*.dll "$PROJECT_DIR/resources/bin/" 2>/dev/null || true
        fi
    else
        echo_error "Fluidsynth URL not found in build config"
        exit 1
    fi
    
    echo_summary "All required Windows binaries downloaded and installed"
}

# Run the build
main "$@"
