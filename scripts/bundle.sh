#!/bin/bash
# Bundle Slopsmith Desktop for distribution
# Creates a self-contained package with Python, Slopsmith server, plugins, and tools

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
SLOPSMITH_DIR="$HOME/Repositories/slopsmith"
BUNDLE_DIR="$PROJECT_DIR/resources/slopsmith"

echo "=== Bundling Slopsmith Desktop ==="
echo ""

# Clean previous bundle
rm -rf "$BUNDLE_DIR"
mkdir -p "$BUNDLE_DIR"

# ── 1. Slopsmith server source ────────────────────────────────────────────
echo "[1/6] Copying Slopsmith server..."
cp "$SLOPSMITH_DIR/server.py" "$BUNDLE_DIR/"
cp -r "$SLOPSMITH_DIR/lib" "$BUNDLE_DIR/"

# Static assets (skip cached audio files and art cache)
mkdir -p "$BUNDLE_DIR/static"
cp "$SLOPSMITH_DIR/static/app.js" "$BUNDLE_DIR/static/"
cp "$SLOPSMITH_DIR/static/highway.js" "$BUNDLE_DIR/static/"
cp "$SLOPSMITH_DIR/static/style.css" "$BUNDLE_DIR/static/"

# ── 2. Plugins ────────────────────────────────────────────────────────────
echo "[2/6] Copying plugins..."
mkdir -p "$BUNDLE_DIR/plugins"

# Built-in plugins
for plugin_dir in "$SLOPSMITH_DIR/plugins/editor" "$SLOPSMITH_DIR/plugins/note_detect"; do
    if [ -d "$plugin_dir" ] && [ ! -L "$plugin_dir" ]; then
        cp -r "$plugin_dir" "$BUNDLE_DIR/plugins/"
    fi
done

# External plugins (resolve symlinks, skip .git)
for plugin_link in "$SLOPSMITH_DIR/plugins/"*; do
    name=$(basename "$plugin_link")
    [ "$name" = "__pycache__" ] && continue
    [ "$name" = "__init__.py" ] && continue
    target="$BUNDLE_DIR/plugins/$name"
    [ -d "$target" ] && continue  # already copied

    if [ -L "$plugin_link" ]; then
        # Resolve symlink and copy actual files (skip .git)
        real_dir=$(readlink -f "$plugin_link")
        if [ -d "$real_dir" ]; then
            mkdir -p "$target"
            rsync -a --exclude='.git' "$real_dir/" "$target/"
        fi
    elif [ -d "$plugin_link" ]; then
        cp -r "$plugin_link" "$target"
    elif [ -f "$plugin_link" ]; then
        cp "$plugin_link" "$target"
    fi
done

# Copy __init__.py for plugin discovery
cp "$SLOPSMITH_DIR/plugins/__init__.py" "$BUNDLE_DIR/plugins/"

# Desktop-specific plugins
for dp in "$PROJECT_DIR/src/renderer" "$PROJECT_DIR/src/renderer/plugin-manager"; do
    if [ -f "$dp/plugin.json" ]; then
        pname=$(python3 -c "import json; print(json.load(open('$dp/plugin.json'))['id'])")
        mkdir -p "$BUNDLE_DIR/plugins/$pname"
        cp "$dp"/*.html "$dp"/*.js "$dp"/plugin.json "$BUNDLE_DIR/plugins/$pname/" 2>/dev/null || true
    fi
done

# ── 3. Python runtime ─────────────────────────────────────────────────────
echo "[3/6] Creating portable Python environment..."
PYTHON_BUNDLE="$PROJECT_DIR/resources/python"
rm -rf "$PYTHON_BUNDLE"

# Create a fresh venv with only the needed packages
python3 -m venv "$PYTHON_BUNDLE"
"$PYTHON_BUNDLE/bin/pip" install --quiet --no-cache-dir \
    fastapi "uvicorn[standard]" websockets pycryptodome pyguitarpro \
    Pillow midiutil python-multipart 2>&1 | tail -3

echo "  Python venv size: $(du -sh "$PYTHON_BUNDLE" | cut -f1)"

# ── 4. System binaries ────────────────────────────────────────────────────
echo "[4/6] Copying system binaries..."
BIN_DIR="$PROJECT_DIR/resources/bin"
mkdir -p "$BIN_DIR"

for tool in ffmpeg vgmstream-cli fluidsynth; do
    tool_path=$(which "$tool" 2>/dev/null)
    if [ -n "$tool_path" ]; then
        cp "$tool_path" "$BIN_DIR/"
        echo "  $tool: $(ls -lh "$BIN_DIR/$tool" | awk '{print $5}')"
    else
        echo "  WARNING: $tool not found"
    fi
done

# Copy required shared libraries for vgmstream
for lib in libvgmstream libjansson libmpg123; do
    found=$(ldconfig -p 2>/dev/null | grep "$lib" | head -1 | awk '{print $NF}')
    if [ -n "$found" ]; then
        cp "$found" "$BIN_DIR/" 2>/dev/null || true
    fi
done

# Pull in fluidsynth's non-system .so dependencies so it runs on end-user
# machines without the libfluidsynth3 (etc.) system packages. Mirrors the
# CI step in .github/workflows/build.yml. Requires patchelf (`sudo apt
# install patchelf`).
if [ -f "$BIN_DIR/fluidsynth" ] && command -v patchelf >/dev/null 2>&1; then
    ldd "$BIN_DIR/fluidsynth" | awk '/=>/ {print $3}' | while read -r lib; do
        [ -n "$lib" ] && [ -f "$lib" ] || continue
        case "$(basename "$lib")" in
            libc.so*|libm.so*|libpthread.so*|libdl.so*|librt.so*|\
            ld-linux*|libresolv.so*|linux-vdso*|linux-gate*|\
            libnsl.so*|libutil.so*|libgcc_s.so*)
                continue
                ;;
        esac
        cp -L "$lib" "$BIN_DIR/" 2>/dev/null || true
    done
    patchelf --set-rpath '$ORIGIN' "$BIN_DIR/fluidsynth"
    for so in "$BIN_DIR"/*.so*; do
        [ -f "$so" ] && patchelf --set-rpath '$ORIGIN' "$so" 2>/dev/null || true
    done
elif [ -f "$BIN_DIR/fluidsynth" ]; then
    echo "  WARNING: patchelf not found — fluidsynth will require system libs at runtime"
fi

# ── 5. Default resources ──────────────────────────────────────────────────
echo "[5/6] Copying default resources..."
# NAM models and IRs stay as user downloads, not bundled
# But copy the default IRs
mkdir -p "$PROJECT_DIR/resources/default-irs"
cp "$PROJECT_DIR/models/cabs/"*.wav "$PROJECT_DIR/resources/default-irs/" 2>/dev/null || true

# Default soundfont (for GP5 → audio rendering). Downloaded if not present;
# CI does the same thing in .github/workflows/build.yml.
SF_DIR="$PROJECT_DIR/resources/soundfonts"
SF_FILE="$SF_DIR/GeneralUser-GS.sf2"
SF_SHA="298b552d2e9d1307e03e5c5c99d2c046aaed9ec3"
mkdir -p "$SF_DIR"
if [ ! -f "$SF_FILE" ]; then
    echo "  downloading GeneralUser-GS.sf2 (32 MB)..."
    curl -sL "https://raw.githubusercontent.com/mrbumpy409/GeneralUser-GS/$SF_SHA/GeneralUser-GS.sf2" -o "$SF_FILE"
fi
echo "  soundfont: $(du -h "$SF_FILE" | cut -f1)"

# ── 6. Summary ────────────────────────────────────────────────────────────
echo "[6/6] Bundle summary:"
echo "  Slopsmith server: $(du -sh "$BUNDLE_DIR" | cut -f1)"
echo "  Python runtime:   $(du -sh "$PYTHON_BUNDLE" | cut -f1)"
echo "  System binaries:  $(du -sh "$BIN_DIR" | cut -f1)"
echo "  Native addon:     $(ls -lh "$PROJECT_DIR/build/Release/slopsmith_audio.node" | awk '{print $5}')"
echo "  Plugins:          $(ls -d "$BUNDLE_DIR/plugins/"*/ 2>/dev/null | wc -l)"
echo ""
TOTAL=$(du -sh "$PROJECT_DIR/resources" | cut -f1)
echo "  Total resources: $TOTAL"
echo ""
echo "Ready for: npm run dist:linux"
