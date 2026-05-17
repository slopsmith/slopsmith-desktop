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
# (Sloppak conversion encodes .ogg with `-c:a libvorbis`).
#   $1 — install hint shown when ffmpeg/ffprobe are missing.
#   $2 — platform-specific remediation shown when libvorbis is absent.
check_media_chain() {
    local install_hint="$1"
    local libvorbis_hint="$2"
    command -v ffmpeg  >/dev/null 2>&1 && echo "  [OK] ffmpeg"  || echo "  [MISSING] ffmpeg ($install_hint)"
    command -v ffprobe >/dev/null 2>&1 && echo "  [OK] ffprobe" || echo "  [MISSING] ffprobe (ships with ffmpeg — $install_hint)"
    if command -v ffmpeg >/dev/null 2>&1; then
        if ffmpeg -hide_banner -encoders 2>/dev/null | grep -wq libvorbis; then
            echo "  [OK] ffmpeg libvorbis encoder"
        else
            echo "  [WARN] ffmpeg lacks the libvorbis encoder — Sloppak conversion"
            echo "         falls back to the lower-quality built-in vorbis encoder."
            echo "         $libvorbis_hint"
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
        check_media_chain "apt: ffmpeg / pacman: ffmpeg" \
            "Most distro ffmpeg packages enable libvorbis — reinstall your distro's ffmpeg if this build does not."
        command -v vgmstream-cli >/dev/null 2>&1 && echo "  [OK] vgmstream-cli" || echo "  [MISSING] vgmstream-cli (AUR: yay -S vgmstream-cli-bin / or github.com/vgmstream/vgmstream/releases)"
        ;;
    Darwin)
        echo ""
        echo "Checking macOS dependencies..."
        xcode-select -p &>/dev/null && echo "  [OK] Xcode Command Line Tools" || echo "  [MISSING] Run: xcode-select --install"
        check_media_chain "brew install ffmpeg" \
            "Homebrew's ffmpeg 8.1.1+ omits libvorbis — install a static ffmpeg build instead (packaged builds bundle one)."
        command -v vgmstream-cli >/dev/null 2>&1 && echo "  [OK] vgmstream-cli" || echo "  [MISSING] vgmstream-cli (brew install vgmstream)"
        ;;
    MINGW*|MSYS*|CYGWIN*)
        echo ""
        echo "Checking Windows (Git Bash) dependencies..."
        check_media_chain "install ffmpeg and add it to PATH (e.g. winget install Gyan.FFmpeg)" \
            "Most prebuilt Windows ffmpeg builds (e.g. Gyan) include libvorbis — pick one that does."
        command -v vgmstream-cli >/dev/null 2>&1 && echo "  [OK] vgmstream-cli" || echo "  [MISSING] vgmstream-cli (github.com/vgmstream/vgmstream/releases — add to PATH)"
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

# Locate Slopsmith. Matches the build scripts (bundle-slopsmith.sh,
# bundle-python.sh, build-macos.sh): an explicit $SLOPSMITH_DIR is honoured
# verbatim — a typo or partial checkout there is surfaced, never silently
# masked by a sibling or legacy checkout. Only when $SLOPSMITH_DIR is unset
# do we fall back to ../slopsmith then ~/Repositories/slopsmith, and a
# fallback candidate only counts if it actually contains server.py.
SLOPSMITH_DIR_ENV="${SLOPSMITH_DIR:-}"
if [ -z "${SLOPSMITH_DIR:-}" ]; then
    if [ -f "$PROJECT_DIR/../slopsmith/server.py" ]; then
        SLOPSMITH_DIR="$PROJECT_DIR/../slopsmith"
    elif [ -f "$HOME/Repositories/slopsmith/server.py" ]; then
        SLOPSMITH_DIR="$HOME/Repositories/slopsmith"
    fi
fi

if [ -n "${SLOPSMITH_DIR:-}" ] && [ -f "$SLOPSMITH_DIR/server.py" ]; then
    SLOPSMITH_DIR="$(cd "$SLOPSMITH_DIR" && pwd)"
    echo ""
    echo "Slopsmith found at: $SLOPSMITH_DIR"

    # On Windows/Git Bash a bash check passes for an MSYS path like
    # /c/src/slopsmith, but `npm run dev` (Electron) resolves $SLOPSMITH_DIR
    # with Node, which needs a native Windows path. Warn before setup
    # reports a config that dev mode would still fail to start.
    case "$(uname -s)" in
        MINGW*|MSYS*|CYGWIN*)
            if [ -n "$SLOPSMITH_DIR_ENV" ] && [ "${SLOPSMITH_DIR_ENV#/}" != "$SLOPSMITH_DIR_ENV" ]; then
                echo ""
                echo "  NOTE: \$SLOPSMITH_DIR is an MSYS/Git-Bash path. 'npm run dev' needs a"
                echo "        native Windows path. Re-export it as:"
                if command -v cygpath >/dev/null 2>&1; then
                    echo "          export SLOPSMITH_DIR='$(cygpath -w "$SLOPSMITH_DIR_ENV")'"
                else
                    echo "          a native path such as C:\\src\\slopsmith"
                fi
            fi
            ;;
    esac

    PYTHON="${PROJECT_DIR}/.venv/bin/python3"
    [ -x "$PYTHON" ] || PYTHON="python3"
    echo "Checking Python dependencies (\"$PYTHON\")..."
    "$PYTHON" -c "import fastapi" 2>/dev/null && echo "  [OK] fastapi" || echo "  [MISSING] \"$PYTHON\" -m pip install -r \"$SLOPSMITH_DIR/requirements.txt\""
    "$PYTHON" -c "import uvicorn" 2>/dev/null && echo "  [OK] uvicorn" || echo "  [MISSING] \"$PYTHON\" -m pip install -r \"$SLOPSMITH_DIR/requirements.txt\""
elif [ -n "${SLOPSMITH_DIR:-}" ]; then
    echo ""
    echo "WARNING: \$SLOPSMITH_DIR is set to '$SLOPSMITH_DIR' but no server.py was found there."
    echo "         Fix the path or unset \$SLOPSMITH_DIR to fall back to ../slopsmith or ~/Repositories/slopsmith."
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
