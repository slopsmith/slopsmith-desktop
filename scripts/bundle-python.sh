#!/bin/bash

# Bundle a portable Python runtime into resources/python/runtime/ for Linux.
#
# Produces a relocatable interpreter + stdlib that the Electron main process
# spawns via `python.ts`. The layout matches python.ts's expectations:
#   resources/python/runtime/bin/python3
#
# Downloads python-build-standalone (same source as actions/setup-python in
# CI) so the result is identical regardless of what Python is installed on the
# host or in the Docker container.
#
# Linux-only: macOS and Windows bundles are handled inline in build-common.sh.

set -euo pipefail

if [ "$(uname -s)" != "Linux" ]; then
    echo "bundle-python.sh is Linux-only. macOS and Windows bundles run inline in CI." >&2
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
CONFIG="$PROJECT_DIR/.build-config.json"

PYTHON_FULL_VERSION=$(python3 "$SCRIPT_DIR/parse-build-config.py" "$CONFIG" .versions.python)
PYTHON_VERSION="${PYTHON_FULL_VERSION%.*}"

PYTHON_STANDALONE_URL=$(python3 "$SCRIPT_DIR/parse-build-config.py" "$CONFIG" .external.python_standalone_linux_x64.url)
PYTHON_STANDALONE_SHA256=$(python3 "$SCRIPT_DIR/parse-build-config.py" "$CONFIG" .external.python_standalone_linux_x64.sha256)

PYTHON_BUNDLE="$PROJECT_DIR/resources/python/runtime"

echo "=== Bundling Python $PYTHON_FULL_VERSION runtime ==="
echo " Downloading python-build-standalone..."

TMPDIR_PBS=$(mktemp -d)
trap 'rm -rf "$TMPDIR_PBS"' EXIT

curl -fsSL "$PYTHON_STANDALONE_URL" -o "$TMPDIR_PBS/python-standalone.tar.gz"
echo "${PYTHON_STANDALONE_SHA256}  $TMPDIR_PBS/python-standalone.tar.gz" | sha256sum -c -
tar -xzf "$TMPDIR_PBS/python-standalone.tar.gz" -C "$TMPDIR_PBS"

PBS_PREFIX="$TMPDIR_PBS/python"

rm -rf "$PROJECT_DIR/resources/python"
mkdir -p "$PYTHON_BUNDLE/bin" "$PYTHON_BUNDLE/lib"

cp "$PBS_PREFIX/bin/python${PYTHON_VERSION}" "$PYTHON_BUNDLE/bin/python3"
chmod +x "$PYTHON_BUNDLE/bin/python3"
cp -r "$PBS_PREFIX/lib/python${PYTHON_VERSION}" "$PYTHON_BUNDLE/lib/"
cp "$PBS_PREFIX/lib"/libpython${PYTHON_VERSION}*.so* "$PYTHON_BUNDLE/lib/"

echo " Bootstrapping pip via ensurepip in the bundled runtime"
LD_LIBRARY_PATH="$PYTHON_BUNDLE/lib" "$PYTHON_BUNDLE/bin/python3" -m ensurepip --upgrade --default-pip

# Resolve the slopsmith repo so we can pip install from its
# requirements.txt — that's the single source of truth for runtime
# deps. Search order matches bundle-slopsmith.sh:
#   1. $SLOPSMITH_DIR env var (set by clone_slopsmith() in CI)
#   2. ../slopsmith (sibling to this repo)
#   3. ~/Repositories/slopsmith (legacy dev layout)
if [ -z "${SLOPSMITH_DIR:-}" ]; then
    if [ -d "$PROJECT_DIR/../slopsmith" ]; then
        SLOPSMITH_DIR="$PROJECT_DIR/../slopsmith"
    elif [ -d "$HOME/Repositories/slopsmith" ]; then
        SLOPSMITH_DIR="$HOME/Repositories/slopsmith"
    fi
fi
if [ -z "${SLOPSMITH_DIR:-}" ] || [ ! -f "$SLOPSMITH_DIR/requirements.txt" ]; then
    echo "ERROR: slopsmith requirements.txt not found." >&2
    echo "Searched:" >&2
    echo "  \$SLOPSMITH_DIR=${SLOPSMITH_DIR:-<unset>}" >&2
    echo "  $PROJECT_DIR/../slopsmith" >&2
    echo "  $HOME/Repositories/slopsmith" >&2
    echo "Clone slopsmith next to this repo: git clone https://github.com/slopsmith/slopsmith.git $PROJECT_DIR/../slopsmith" >&2
    exit 1
fi

echo " Installing slopsmith runtime requirements ($SLOPSMITH_DIR/requirements.txt)"
LD_LIBRARY_PATH="$PYTHON_BUNDLE/lib" "$PYTHON_BUNDLE/bin/python3" -m pip install --quiet --no-cache-dir \
    -r "$SLOPSMITH_DIR/requirements.txt" 2>&1 | tail -3

echo " Installing desktop-only Python extras"
LD_LIBRARY_PATH="$PYTHON_BUNDLE/lib" "$PYTHON_BUNDLE/bin/python3" -m pip install --quiet --no-cache-dir \
    -r "$PROJECT_DIR/.packages/python.txt" 2>&1 | tail -3

echo " Python runtime size: $(du -sh "$PYTHON_BUNDLE" | cut -f1)"
echo "=== Python bundle complete ==="
