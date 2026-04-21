#!/bin/bash
# Development environment setup
# Installs all dependencies and verifies the build chain

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

cd "$PROJECT_DIR"

echo "=== Slopsmith Desktop Development Setup ==="
echo ""

# Check prerequisites
echo "Checking prerequisites..."

check_command() {
    if command -v "$1" &>/dev/null; then
        echo "  [OK] $1"
    else
        echo "  [MISSING] $1 — $2"
        return 1
    fi
}

check_command node "Install Node.js 20+" || exit 1
check_command npm "Comes with Node.js" || exit 1
check_command cmake "Install cmake (apt/brew/pacman)" || exit 1
check_command python3 "Install Python 3.12+" || exit 1
check_command git "Install git" || exit 1

# Platform-specific checks
case "$(uname -s)" in
    Linux)
        echo ""
        echo "Checking Linux build dependencies..."
        pkg-config --exists alsa 2>/dev/null && echo "  [OK] ALSA" || echo "  [MISSING] ALSA dev headers (apt: libasound2-dev / pacman: alsa-lib)"
        pkg-config --exists jack 2>/dev/null && echo "  [OK] JACK" || echo "  [MISSING] JACK dev headers (apt: libjack-jackd2-dev / pacman: jack2)"
        pkg-config --exists freetype2 2>/dev/null && echo "  [OK] freetype2" || echo "  [MISSING] freetype2 (apt: libfreetype-dev / pacman: freetype2)"
        pkg-config --exists x11 2>/dev/null && echo "  [OK] X11" || echo "  [MISSING] X11 dev headers"
        pkg-config --exists xrandr 2>/dev/null && echo "  [OK] Xrandr" || echo "  [MISSING] Xrandr dev headers"
        pkg-config --exists xcursor 2>/dev/null && echo "  [OK] Xcursor" || echo "  [MISSING] Xcursor dev headers"
        pkg-config --exists xinerama 2>/dev/null && echo "  [OK] Xinerama" || echo "  [MISSING] Xinerama dev headers"
        ;;
    Darwin)
        echo ""
        echo "Checking macOS dependencies..."
        xcode-select -p &>/dev/null && echo "  [OK] Xcode Command Line Tools" || echo "  [MISSING] Run: xcode-select --install"
        ;;
esac

echo ""

# Initialize submodules
echo "Initializing git submodules..."
git submodule update --init --recursive 2>/dev/null || echo "  Note: Run 'git submodule update --init --recursive' manually if this is a fresh clone"

# Install npm dependencies
echo ""
echo "Installing npm dependencies..."
npm install

# Check Slopsmith repo
# Priority: 1) ../slopsmith (relative), 2) ~/Repositories/slopsmith (legacy)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

if [ -d "$PROJECT_DIR/../slopsmith" ]; then
    SLOPSMITH_DIR="$PROJECT_DIR/../slopsmith"
elif [ -d "$HOME/Repositories/slopsmith" ]; then
    SLOPSMITH_DIR="$HOME/Repositories/slopsmith"
else
    SLOPSMITH_DIR=""
fi

if [ -n "$SLOPSMITH_DIR" ]; then
    echo ""
    echo "Slopsmith found at: $SLOPSMITH_DIR"

    # Check Python deps
    echo "Checking Python dependencies..."
    python3 -c "import fastapi" 2>/dev/null && echo "  [OK] fastapi" || echo "  [MISSING] pip install fastapi"
    python3 -c "import uvicorn" 2>/dev/null && echo "  [OK] uvicorn" || echo "  [MISSING] pip install uvicorn[standard]"
else
    echo ""
    echo "WARNING: Slopsmith not found."
    echo "Searched locations:"
    echo "  - $PROJECT_DIR/../slopsmith (relative to this repo)"
    echo "  - $HOME/Repositories/slopsmith (legacy location)"
    echo ""
    echo "Clone it: git clone https://github.com/byrongamatos/slopsmith.git ../slopsmith"
fi

echo ""
echo "=== Setup Complete ==="
echo ""
echo "Build commands:"
echo "  npm run build:audio     # Build JUCE native addon"
echo "  npm run build:ts        # Compile TypeScript"
echo "  npm run dev             # Run in development mode"
echo "  npm run dist:linux      # Build Linux package"
