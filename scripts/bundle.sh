#!/bin/bash
# Top-level bundle delegator. Calls the per-concern modular scripts so
# local dev matches CI. Linux-focused; macOS/Windows bundling lives in
# the GitHub Actions workflow because those platforms have quite
# different packaging needs (dylibbundler, zip downloads, etc.).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

echo "=== Bundling Slopsmith Desktop ==="

bash "$SCRIPT_DIR/bundle-slopsmith.sh"
# Skip Python bundling on non-Linux platforms (handled inline in platform scripts)
if [[ "$(uname -s)" == "Linux" ]]; then
  bash "$SCRIPT_DIR/bundle-python.sh"
fi
# Skip binary bundling on non-Linux platforms (handled inline in platform scripts)
if [[ "$(uname -s)" == "Linux" ]]; then
  bash "$SCRIPT_DIR/bundle-binaries.sh"
fi
bash "$SCRIPT_DIR/bundle-soundfont.sh"

# Default IRs — small copy step that doesn't need its own script.
echo "=== Copying default IRs ==="
mkdir -p "$PROJECT_DIR/resources/default-irs"
cp "$PROJECT_DIR/models/cabs/"*.wav "$PROJECT_DIR/resources/default-irs/" 2>/dev/null || true
echo "  Default IRs: $(ls "$PROJECT_DIR/resources/default-irs/" | wc -l) file(s)"

echo ""
echo "=== Bundle complete ==="
echo "  Total resources: $(du -sh "$PROJECT_DIR/resources" | cut -f1)"
echo ""
echo "Ready for: npm run dist"
