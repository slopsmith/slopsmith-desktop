#!/bin/bash
# Bundle the Slopsmith server source + plugins into resources/slopsmith/.
#
# Slopsmith repo location is resolved in this order:
#   1. $SLOPSMITH_DIR env var
#   2. ../slopsmith (sibling to this repo)
#   3. ~/Repositories/slopsmith (legacy dev layout)
#
# Cross-platform: avoids `readlink -f` (not available on macOS by default)
# by using python's os.path.realpath. `rsync` is used for the resolved-
# symlink copy step and must be present (see .packages/apt.txt).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUNDLE_DIR="$PROJECT_DIR/resources/slopsmith"

if [ -z "${SLOPSMITH_DIR:-}" ]; then
    if [ -d "$PROJECT_DIR/../slopsmith" ]; then
        SLOPSMITH_DIR="$PROJECT_DIR/../slopsmith"
    elif [ -d "$HOME/Repositories/slopsmith" ]; then
        SLOPSMITH_DIR="$HOME/Repositories/slopsmith"
    else
        SLOPSMITH_DIR=""
    fi
fi

if [ -z "$SLOPSMITH_DIR" ] || [ ! -d "$SLOPSMITH_DIR" ]; then
    echo "ERROR: Slopsmith repository not found." >&2
    echo "Searched:" >&2
    echo "  \$SLOPSMITH_DIR (unset)" >&2
    echo "  $PROJECT_DIR/../slopsmith" >&2
    echo "  $HOME/Repositories/slopsmith" >&2
    echo "Clone it with: git clone https://github.com/slopsmith/slopsmith.git ../slopsmith" >&2
    exit 1
fi

# Portable realpath — readlink -f doesn't exist on stock macOS.
realpath_portable() {
    python3 -c 'import os, sys; print(os.path.realpath(sys.argv[1]))' "$1"
}

echo "=== Bundling Slopsmith server and plugins ==="
echo "  Source: $SLOPSMITH_DIR"

rm -rf "$BUNDLE_DIR"
mkdir -p "$BUNDLE_DIR/static" "$BUNDLE_DIR/plugins"

# Server + lib
cp "$SLOPSMITH_DIR/server.py" "$BUNDLE_DIR/"
cp "$SLOPSMITH_DIR/VERSION" "$BUNDLE_DIR/"
cp -r "$SLOPSMITH_DIR/lib" "$BUNDLE_DIR/"
rm -rf "$BUNDLE_DIR/lib/__pycache__"

# Static assets — copy the whole directory. User-data dirs (art/, sloppak_cache/)
# and generated audio_*.mp3 files are gitignored and won't exist in a clean checkout.
cp -r "$SLOPSMITH_DIR/static/." "$BUNDLE_DIR/static/"
# Strip any leftover user-data that may exist in a dev checkout.
rm -rf "$BUNDLE_DIR/static/art" "$BUNDLE_DIR/static/sloppak_cache"
find "$BUNDLE_DIR/static" -maxdepth 1 -name 'audio_*.mp3' -delete

# Cross-platform "cp -r minus .git" — Git Bash on Windows doesn't ship
# rsync, so we can't rely on `rsync --exclude=.git`. Plain `cp -r`
# followed by stripping any nested `.git/` directories works on
# Linux/macOS/Git-Bash alike. The .git stripping matters because
# plugin directories cloned by clone_slopsmith() are git working trees;
# bundling their .git/objects/ would inflate the .app and (on macOS)
# trip electron-builder with EACCES on read-only pack files.
copy_plugin() {
    local src="$1"
    local dst="$2"
    mkdir -p "$dst"
    # Use cp -R rather than -r for portable symlink-following semantics.
    cp -R "$src/." "$dst/"
    find "$dst" -name '.git' -type d -prune -exec rm -rf {} +
}

# Built-in plugins (real directories, not symlinks to avoid duplicates).
for plugin_dir in "$SLOPSMITH_DIR/plugins/editor" "$SLOPSMITH_DIR/plugins/note_detect"; do
    if [ -d "$plugin_dir" ] && [ ! -L "$plugin_dir" ]; then
        name=$(basename "$plugin_dir")
        copy_plugin "$plugin_dir" "$BUNDLE_DIR/plugins/$name"
    fi
done

# External plugins: resolve symlinks, skip .git
for plugin_link in "$SLOPSMITH_DIR/plugins/"*; do
    name=$(basename "$plugin_link")
    [ "$name" = "__pycache__" ] && continue
    [ "$name" = "__init__.py" ] && continue
    target="$BUNDLE_DIR/plugins/$name"
    [ -d "$target" ] && continue  # already copied

    if [ -L "$plugin_link" ]; then
        real_dir=$(realpath_portable "$plugin_link")
        if [ -d "$real_dir" ]; then
            copy_plugin "$real_dir" "$target"
        fi
    elif [ -d "$plugin_link" ]; then
        copy_plugin "$plugin_link" "$target"
    elif [ -f "$plugin_link" ]; then
        cp "$plugin_link" "$target"
    fi
done

# Plugin-discovery __init__.py
cp "$SLOPSMITH_DIR/plugins/__init__.py" "$BUNDLE_DIR/plugins/"

# Desktop-specific plugins (audio_engine, plugin_manager) declared in
# src/renderer/**/plugin.json
for dp in "$PROJECT_DIR/src/renderer" "$PROJECT_DIR/src/renderer/plugin-manager"; do
    if [ -f "$dp/plugin.json" ]; then
        pname=$(python3 -c "import json, sys; print(json.load(open(sys.argv[1]))['id'])" "$dp/plugin.json")
        mkdir -p "$BUNDLE_DIR/plugins/$pname"
        cp "$dp"/*.html "$dp"/*.js "$dp"/plugin.json "$BUNDLE_DIR/plugins/$pname/" 2>/dev/null || true
    fi
done

echo "  Slopsmith server: $(du -sh "$BUNDLE_DIR" | cut -f1)"
echo "  Plugins: $(ls -d "$BUNDLE_DIR/plugins/"*/ 2>/dev/null | wc -l)"
echo "=== Slopsmith bundle complete ==="
