#!/bin/bash
# Native Windows build script
# Runs in Git Bash (Git for Windows)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -W 2>/dev/null || pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
CONFIG="$PROJECT_DIR/.build-config.json"

# Platform-specific: Return expected artifact patterns
get_expected_artifacts() {
	echo "$PROJECT_DIR/release/*.exe $PROJECT_DIR/release/*.zip"
}

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
	curl -sL --fail --retry 5 --retry-delay 5 --retry-all-errors "$PYTHON_EMBED_URL" -o /tmp/python-embed.zip

	mkdir -p "$PROJECT_DIR/resources/python"
	unzip -q /tmp/python-embed.zip -d "$PROJECT_DIR/resources/python/"

	# Enable site-packages by editing the ._pth file
	PTH_FILE=$(find "$PROJECT_DIR/resources/python" -name "*.pth" | head -1)
	if [[ -n "$PTH_FILE" ]]; then
		sed -i 's/#import site/import site/' "$PTH_FILE"
		echo "Lib/site-packages" >> "$PTH_FILE"
	fi

	# Install pip
	curl -sL --fail --retry 5 --retry-delay 5 --retry-all-errors \
		https://bootstrap.pypa.io/get-pip.py -o /tmp/get-pip.py
	"$PROJECT_DIR/resources/python/python.exe" /tmp/get-pip.py --quiet --no-cache-dir

	# Install packages
	"$PROJECT_DIR/resources/python/python.exe" -m pip install --quiet --no-cache-dir setuptools wheel
	"$PROJECT_DIR/resources/python/python.exe" -m pip install --quiet --no-cache-dir \
		fastapi "uvicorn[standard]" websockets pycryptodome pyguitarpro \
		Pillow midiutil python-multipart requests

	# Remove ._pth for PYTHONPATH support
	rm -f "$PTL_FILE"
}

# Platform-specific: Bundle system binaries
bundle_binaries_impl() {
	mkdir -p "$PROJECT_DIR/resources/bin"
	local CURL="curl -sL --fail --retry 5 --retry-delay 5 --retry-all-errors"

	# ffmpeg static build
	echo "Downloading ffmpeg..."
	$CURL "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-win64-gpl.zip" \
		-o /tmp/ffmpeg.zip
	unzip -q /tmp/ffmpeg.zip -d /tmp/ffmpeg
	cp /tmp/ffmpeg/ffmpeg-master-latest-win64-gpl/bin/ffmpeg.exe "$PROJECT_DIR/resources/bin/" 2>/dev/null || true

	# vgmstream-cli
	echo "Downloading vgmstream-cli..."
	$CURL "https://github.com/vgmstream/vgmstream/releases/latest/download/vgmstream-win64.zip" \
		-o /tmp/vgmstream.zip
	unzip -q /tmp/vgmstream.zip -d /tmp/vgmstream
	cp /tmp/vgmstream/vgmstream-cli.exe "$PROJECT_DIR/resources/bin/" 2>/dev/null || true
	cp /tmp/vgmstream/*.dll "$PROJECT_DIR/resources/bin/" 2>/dev/null || true

	# fluidsynth
	echo "Downloading fluidsynth..."
	FS_URL=$(python3 -c "import json; print(json.load(open('$CONFIG')).get('external', {}).get('fluidsynth_windows', {}).get('url', ''))" 2>/dev/null)
	if [[ -n "$FS_URL" ]]; then
		$CURL "$FS_URL" -o /tmp/fluidsynth.zip
		unzip -q /tmp/fluidsynth.zip -d /tmp/fluidsynth
		FS_BIN=$(find /tmp/fluidsynth -name 'fluidsynth.exe' -type f | head -1)
		if [[ -n "$FS_BIN" ]]; then
			cp "$FS_BIN" "$PROJECT_DIR/resources/bin/"
			cp "$(dirname "$FS_BIN")"/*.dll "$PROJECT_DIR/resources/bin/" 2>/dev/null || true
		fi
	fi
}

# Source common build logic
source "$SCRIPT_DIR/build-common.sh"

# Run the build
main "$@"
