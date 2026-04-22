#!/bin/bash
# Bundle a portable Python runtime into resources/python/runtime/ for Linux.
#
# Produces a relocatable interpreter + stdlib that the Electron main process
# spawns via `python.ts`. The layout matches python.ts's expectations:
#   resources/python/runtime/bin/python3
#
# Linux-only: macOS and Windows CI use inline logic in build.yml because
# their Python-bundling strategies are quite different (setup-python copy
# on macOS; embeddable zip on Windows).

set -euo pipefail

if [ "$(uname -s)" != "Linux" ]; then
    echo "bundle-python.sh is Linux-only. macOS and Windows bundles run inline in CI." >&2
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
CONFIG="$PROJECT_DIR/.build-config.json"

PYTHON_VERSION=$(python3 "$SCRIPT_DIR/parse-build-config.py" "$CONFIG" .versions.python)
PYTHON_BUNDLE="$PROJECT_DIR/resources/python/runtime"

echo "=== Bundling Python $PYTHON_VERSION runtime ==="

# Locate the requested Python interpreter.
if command -v "python${PYTHON_VERSION}" >/dev/null 2>&1; then
    SYS_PYTHON="python${PYTHON_VERSION}"
elif command -v python3 >/dev/null 2>&1; then
    SYS_PYTHON="python3"
    detected=$(python3 -c 'import sys; print(f"{sys.version_info.major}.{sys.version_info.minor}")')
    if [ "$detected" != "$PYTHON_VERSION" ]; then
        echo "  WARNING: requested Python $PYTHON_VERSION, found python3 reporting $detected — continuing with $SYS_PYTHON" >&2
    fi
else
    echo "ERROR: no python3 available on PATH" >&2
    exit 1
fi

PYTHON_PREFIX=$("$SYS_PYTHON" -c 'import sys; print(sys.prefix)')
echo "  Source Python: $PYTHON_PREFIX"

rm -rf "$PROJECT_DIR/resources/python"
mkdir -p "$PYTHON_BUNDLE/bin" "$PYTHON_BUNDLE/lib"

# Copy interpreter + stdlib.
cp "$PYTHON_PREFIX/bin/python${PYTHON_VERSION}" "$PYTHON_BUNDLE/bin/python3"
chmod +x "$PYTHON_BUNDLE/bin/python3"
cp -r "$PYTHON_PREFIX/lib/python${PYTHON_VERSION}" "$PYTHON_BUNDLE/lib/"

# Copy libpython shared library — Python depends on it at runtime.
PYTHON_LIBDIR=$("$SYS_PYTHON" -c 'import sysconfig; print(sysconfig.get_config_var("LIBDIR"))')
cp "$PYTHON_LIBDIR"/libpython${PYTHON_VERSION}*.so* "$PYTHON_BUNDLE/lib/" 2>/dev/null || true

# Bootstrap pip INSIDE the bundled runtime. Using the system Python to
# install pip (the old approach) put pip on the host, not here; packages
# then got installed into the wrong place.
echo "  Bootstrapping pip via ensurepip in the bundled runtime"
LD_LIBRARY_PATH="$PYTHON_BUNDLE/lib" "$PYTHON_BUNDLE/bin/python3" -m ensurepip --upgrade --default-pip

# Install application packages into the bundled runtime.
echo "  Installing application packages"
LD_LIBRARY_PATH="$PYTHON_BUNDLE/lib" "$PYTHON_BUNDLE/bin/python3" -m pip install --quiet --no-cache-dir \
    fastapi "uvicorn[standard]" websockets pycryptodome pyguitarpro \
    Pillow midiutil python-multipart requests 2>&1 | tail -3

echo "  Python runtime size: $(du -sh "$PYTHON_BUNDLE" | cut -f1)"
echo "=== Python bundle complete ==="
