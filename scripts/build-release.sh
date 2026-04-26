#!/bin/bash
# Unified release build script that dispatches to platform-specific scripts.
# User runs this script regardless of platform, and it calls the appropriate
# platform-specific build script.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

# Color output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

echo "=== Slopsmith Desktop Build ==="
echo ""

# Detect platform
PLATFORM=""
case "$(uname -s)" in
Linux*)
  PLATFORM="linux"
  ;;
Darwin*)
  PLATFORM="macos"
  ;;
MINGW*|MSYS*|CYGWIN*)
  PLATFORM="windows"
  ;;
*)
  echo -e "${RED}Error: Unsupported platform: $(uname -s)${NC}" >&2
  echo "Supported platforms: Linux, macOS, Windows (Git Bash)" >&2
  exit 1
  ;;
esac

echo -e "${GREEN}Platform:${NC} $PLATFORM"
echo ""

case "$PLATFORM" in
  linux)
    # Check if running on Ubuntu
    if [[ -f /etc/os-release ]] && grep -q '^ID=ubuntu' /etc/os-release; then
      # Ubuntu: Use native Ubuntu build
      if [[ -f "$SCRIPT_DIR/build-linux-ubuntu.sh" ]]; then
        bash "$SCRIPT_DIR/build-linux-ubuntu.sh"
      else
        echo -e "${RED}Error: build-linux-ubuntu.sh not found${NC}" >&2
        exit 1
      fi
    else
      # Other Linux: Use Docker for reproducibility across distros
      if [[ -f "$SCRIPT_DIR/build-linux-docker.sh" ]]; then
        bash "$SCRIPT_DIR/build-linux-docker.sh"
      else
        echo -e "${RED}Error: build-linux-docker.sh not found${NC}" >&2
        exit 1
      fi
    fi
    ;;
  macos)
    if [[ -f "$SCRIPT_DIR/build-macos.sh" ]]; then
      bash "$SCRIPT_DIR/build-macos.sh"
    else
      echo -e "${RED}Error: build-macos.sh not found${NC}" >&2
      exit 1
    fi
    ;;
  windows)
    if [[ -f "$SCRIPT_DIR/build-windows.sh" ]]; then
      bash "$SCRIPT_DIR/build-windows.sh"
    else
      echo -e "${RED}Error: build-windows.sh not found${NC}" >&2
      exit 1
    fi
    ;;
  *)
    echo -e "${RED}Error: Unexpected platform: $PLATFORM${NC}" >&2
    exit 1
    ;;
esac

# The exit code from the platform build script

if [[ $exit_code -eq 0 ]]; then
  echo ""
  echo -e "${GREEN}✓${NC} Build complete!"
  echo "Artifacts: $PROJECT_DIR/release/"
else
  echo ""
  echo -e "${RED}✗${NC} Build failed"
fi

exit $exit_code
