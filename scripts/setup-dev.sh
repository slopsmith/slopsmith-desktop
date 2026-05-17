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

# Verify the media chain: ffmpeg (WEM/OGG transcode), ffprobe (demucs probes
# stream metadata before invoking ffmpeg), and ffmpeg's libvorbis encoder
# (Sloppak conversion encodes .ogg with `-c:a libvorbis`). $1 is the install
# hint shown when ffmpeg/ffprobe are missing.
check_media_chain() {
    local install_hint="$1"
    command -v ffmpeg  >/dev/null 2>&1 && echo "  [OK] ffmpeg"  || echo "  [MISSING] ffmpeg ($install_hint)"
    command -v ffprobe >/dev/null 2>&1 && echo "  [OK] ffprobe" || echo "  [MISSING] ffprobe (ships with ffmpeg — $install_hint)"
    if command -v ffmpeg >/dev/null 2>&1; then
        if ffmpeg -hide_banner -encoders 2>/dev/null | grep -wq libvorbis; then
            echo "  [OK] ffmpeg libvorbis encoder"
        else
            echo "  [WARN] ffmpeg lacks the libvorbis encoder — Sloppak conversion"
            echo "         falls back to the lower-quality built-in vorbis encoder."
            echo "         Homebrew's ffmpeg 8.1.1+ dropped libvorbis; packaged builds"
            echo "         bundle a static ffmpeg with --enable-libvorbis instead."
        fi
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
        check_media_chain "apt: ffmpeg / pacman: ffmpeg"
        command -v vgmstream-cli >/dev/null 2>&1 && echo "  [OK] vgmstream-cli" || echo "  [MISSING] vgmstream-cli (AUR: yay -S vgmstream-cli-bin / or github.com/vgmstream/vgmstream/releases)"
        ;;
    Darwin)
        echo ""
        echo "Checking macOS dependencies..."
        xcode-select -p &>/dev/null && echo "  [OK] Xcode Command Line Tools" || echo "  [MISSING] Run: xcode-select --install"
        check_media_chain "brew install ffmpeg"
        command -v vgmstream-cli >/dev/null 2>&1 && echo "  [OK] vgmstream-cli" || echo "  [MISSING] vgmstream-cli (brew install vgmstream)"
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

# Locate Slopsmith: $SLOPSMITH_DIR env, ../slopsmith (sibling), ~/Repositories/slopsmith.
# A candidate only counts if it actually contains server.py — a partial or
# unrelated ../slopsmith directory must not mask a valid legacy checkout.
SLOPSMITH_FOUND=""
for candidate in "${SLOPSMITH_DIR:-}" "$PROJECT_DIR/../slopsmith" "$HOME/Repositories/slopsmith"; do
    [ -n "$candidate" ] || continue
    if [ -f "$candidate/server.py" ]; then
        SLOPSMITH_FOUND="$(cd "$candidate" && pwd)"
        break
    fi
done
SLOPSMITH_DIR="$SLOPSMITH_FOUND"

if [ -n "$SLOPSMITH_DIR" ]; then
    echo ""
    echo "Slopsmith found at: $SLOPSMITH_DIR"

    PYTHON="${PROJECT_DIR}/.venv/bin/python3"
    [ -x "$PYTHON" ] || PYTHON="python3"
    echo "Checking Python dependencies (\"$PYTHON\")..."
    "$PYTHON" -c "import fastapi" 2>/dev/null && echo "  [OK] fastapi" || echo "  [MISSING] \"$PYTHON\" -m pip install -r \"$SLOPSMITH_DIR/requirements.txt\""
    "$PYTHON" -c "import uvicorn" 2>/dev/null && echo "  [OK] uvicorn" || echo "  [MISSING] \"$PYTHON\" -m pip install -r \"$SLOPSMITH_DIR/requirements.txt\""
else
    echo ""
    echo "WARNING: Slopsmith not found. Set \$SLOPSMITH_DIR, clone to $PROJECT_DIR/../slopsmith, or use ~/Repositories/slopsmith"
fi

echo ""
echo "=== Setup Complete ==="
echo ""
echo "Build commands:"
echo "  npm run build:audio     # Build JUCE native addon"
echo "  npm run build:ts        # Compile TypeScript"
echo "  npm run dev             # Run in development mode"
echo "  npm run dist:linux      # Build Linux package"
