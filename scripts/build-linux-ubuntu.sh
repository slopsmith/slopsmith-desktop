#!/bin/bash
# Native Ubuntu build script
# Assumes host is running Ubuntu Linux
# Uses native Ubuntu packages (apt) and copies system Python

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
CONFIG="$PROJECT_DIR/.build-config.json"

# Platform identifier
export PLATFORM="linux"

# Check we're on Linux
if [[ ! -f /etc/os-release ]] || ! grep -q "ubuntu\|debian" /etc/os-release; then
	echo "Note: This script is optimized for Ubuntu/Debian but may work on other distributions."
	echo "For non-Ubuntu Linux, system dependencies might need manual installation."
	echo ""
fi

echo "=== Slopsmith Desktop Ubuntu Native Build ==="
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
	printf "%s\n" "$PROJECT_DIR/release/*.AppImage" "$PROJECT_DIR/release/*.deb"
}

# Platform-specific: Install system dependencies
install_system_deps() {
	if command -v sudo &>/dev/null; then
		sudo apt-get update
		PACKAGES=$(grep -v '^[[:space:]]*#' "$PROJECT_DIR/.packages/apt.txt" | grep -v '^[[:space:]]*$' | tr '\n' ' ')
		if [[ -n "$PACKAGES" ]]; then
			sudo apt-get install -y $PACKAGES
		fi
	else
		echo -e "${YELLOW}!${NC} sudo not available, skipping apt package installation"
		echo "  Make sure build dependencies are already installed"
	fi
}

# Platform-specific: Bundle Python runtime
bundle_python_impl() {
	# Linux: use existing bundle-python.sh script
	bash "$SCRIPT_DIR/bundle-python.sh"
}

# Platform-specific: Bundle system binaries
bundle_binaries_impl() {
	# Linux: use existing bundle-binaries.sh script
	bash "$SCRIPT_DIR/bundle-binaries.sh"
}

# Run the build
main "$@"
