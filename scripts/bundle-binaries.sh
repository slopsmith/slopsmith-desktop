#!/bin/bash
# Bundle system binaries into resources/bin/
# These are required for audio processing (ffmpeg, vgmstream)

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BIN_DIR="$PROJECT_DIR/resources/bin"

echo "=== Bundling system binaries ==="

mkdir -p "$BIN_DIR"

# Copy binaries from .packages/binaries.txt if it exists
# Otherwise fall back to common tools
if [ -f "$PROJECT_DIR/.packages/binaries.txt" ]; then
    while IFS= read -r line || [[ -n "$line" ]]; do
        # Skip comments and empty lines
        [[ "$line" =~ ^[[:space:]]*# ]] && continue
        [[ -z "$line" ]] && continue
        
        tool=$(echo "$line" | awk '{print $1}')
        tool_path=$(which "$tool" 2>/dev/null)
        if [ -n "$tool_path" ]; then
            cp "$tool_path" "$BIN_DIR/"
            echo "  $tool: $(ls -lh "$BIN_DIR/$tool" | awk '{print $5}')"
        else
            echo "  WARNING: $tool not found"
        fi
    done < "$PROJECT_DIR/.packages/binaries.txt"
else
    # Fallback: common tools
    for tool in ffmpeg vgmstream-cli fluidsynth; do
        tool_path=$(which "$tool" 2>/dev/null)
        if [ -n "$tool_path" ]; then
            cp "$tool_path" "$BIN_DIR/"
            echo "  $tool: $(ls -lh "$BIN_DIR/$tool" | awk '{print $5}')"
        else
            echo "  WARNING: $tool not found"
        fi
    done
fi

# Copy required shared libraries for vgmstream
for lib in libvgmstream libjansson libmpg123; do
    found=$(ldconfig -p 2>/dev/null | grep "$lib" | head -1 | awk '{print $NF}')
    if [ -n "$found" ]; then
        cp "$found" "$BIN_DIR/" 2>/dev/null || true
    fi
done

echo "  System binaries: $(du -sh "$BIN_DIR" | cut -f1)"
echo "=== Binary bundle complete ==="
