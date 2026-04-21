#!/bin/bash
# Bundle Slopsmith server and plugins into resources
# This script copies the Slopsmith server code and plugins from the slopsmith repository

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUNDLE_DIR="$PROJECT_DIR/resources/slopsmith"

# Determine Slopsmith directory location
# Priority: 1) Environment variable, 2) ../slopsmith (relative), 3) ~/Repositories/slopsmith (legacy)
if [ -n "$SLOPSMITH_DIR" ]; then
    :  # SLOPSMITH_DIR already set
elif [ -d "$PROJECT_DIR/../slopsmith" ]; then
    SLOPSMITH_DIR="$PROJECT_DIR/../slopsmith"
else
    SLOPSMITH_DIR="$HOME/Repositories/slopsmith"
fi

# Check if Slopsmith repository exists
if [ ! -d "$SLOPSMITH_DIR" ]; then
    echo "ERROR: Slopsmith repository not found."
    echo ""
    echo "Searched locations:"
    echo "  - $PROJECT_DIR/../slopsmith (relative to this repo)"
    echo "  - $HOME/Repositories/slopsmith (legacy location)"
    echo ""
    echo "Clone it: git clone https://github.com/byrongamatos/slopsmith.git ../slopsmith"
    exit 1
fi

echo "=== Bundling Slopsmith server and plugins ==="

# Clean previous bundle
rm -rf "$BUNDLE_DIR"
mkdir -p "$BUNDLE_DIR"

# Copy Slopsmith server source
echo "  Copying server code..."
cp "$SLOPSMITH_DIR/server.py" "$BUNDLE_DIR/"
cp -r "$SLOPSMITH_DIR/lib" "$BUNDLE_DIR/"

# Static assets
mkdir -p "$BUNDLE_DIR/static"
cp "$SLOPSMITH_DIR/static/index.html" "$BUNDLE_DIR/static/"
cp "$SLOPSMITH_DIR/static/app.js" "$BUNDLE_DIR/static/"
cp "$SLOPSMITH_DIR/static/highway.js" "$BUNDLE_DIR/static/"
cp "$SLOPSMITH_DIR/static/style.css" "$BUNDLE_DIR/static/"

# Plugins
echo "  Copying plugins..."
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
    [ -d "$target" ] && continue

    if [ -L "$plugin_link" ]; then
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

echo "  Slopsmith server: $(du -sh "$BUNDLE_DIR" | cut -f1)"
echo "  Plugins: $(ls -d "$BUNDLE_DIR/plugins/"*/ 2>/dev/null | wc -l)"
echo "=== Slopsmith bundle complete ==="
