#!/bin/bash
# Bundle portable Python environment for Linux
# Creates a relocatable Python runtime in resources/python/runtime
# This matches the CI workflow approach

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
PYTHON_BUNDLE="$PROJECT_DIR/resources/python/runtime"

echo "=== Bundling Python environment ==="

# Clean previous bundle
rm -rf "$PROJECT_DIR/resources/python"
mkdir -p "$PYTHON_BUNDLE"

# Get Python 3.12 paths
PYTHON_PREFIX=$(python3.12 -c "import sys; print(sys.prefix)")

echo "  Copying Python from: $PYTHON_PREFIX"

# Create directory structure
mkdir -p "$PYTHON_BUNDLE/bin"
mkdir -p "$PYTHON_BUNDLE/lib"

# Copy Python binary
cp "$PYTHON_PREFIX/bin/python3.12" "$PYTHON_BUNDLE/bin/python3"
chmod +x "$PYTHON_BUNDLE/bin/python3"

# Copy the entire Python 3.12 standard library (not just a venv)
# This includes encodings, site-packages, and all stdlib modules
cp -r "$PYTHON_PREFIX/lib/python3.12" "$PYTHON_BUNDLE/lib/"

# Copy additional Python directories if they exist
for pydir in python3 dist-packages python3.12/dist-packages python3/dist-packages; do
    if [ -d "$PYTHON_PREFIX/lib/$pydir" ]; then
        echo "  Copying $pydir..."
        mkdir -p "$PYTHON_BUNDLE/lib/$(dirname $pydir)"
        cp -r "$PYTHON_PREFIX/lib/$pydir" "$PYTHON_BUNDLE/lib/$pydir" 2>/dev/null || true
    fi
done

# Copy distutils module (required by pip) - it's in the standard library
# but may be in different locations on different systems
if [ -d "/usr/lib/python3.12/distutils" ]; then
    echo "  Copying distutils..."
    cp -r /usr/lib/python3.12/distutils "$PYTHON_BUNDLE/lib/python3.12/" 2>/dev/null || true
fi

# Also copy distutils from python3-distutils if it exists
if [ -d "/usr/lib/python3/dist-packages/distutils" ]; then
    echo "  Copying distutils from dist-packages..."
    cp -r /usr/lib/python3/dist-packages/distutils "$PYTHON_BUNDLE/lib/python3.12/" 2>/dev/null || true
fi

# Copy libpython shared library
PYTHON_LIBDIR=$(python3.12 -c "import sysconfig; print(sysconfig.get_config_var('LIBDIR'))")
cp "$PYTHON_LIBDIR"/libpython3.12*.so* "$PYTHON_BUNDLE/lib/" 2>/dev/null || true

# Install pip for Python 3.12 using get-pip.py
echo "  Installing pip for Python 3.12..."
curl -fsSL https://bootstrap.pypa.io/get-pip.py | python3.12 - --quiet

# Create pip wrapper script that uses --target
cat > "$PYTHON_BUNDLE/bin/pip" << 'EOF'
#!/bin/bash
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
"$SCRIPT_DIR/python3" -m pip "$@"
EOF
chmod +x "$PYTHON_BUNDLE/bin/pip"

# Ensure dist-packages directory exists (Debian/Ubuntu uses this)
mkdir -p "$PYTHON_BUNDLE/lib/python3.12/dist-packages"

# Install packages directly into dist-packages (not site-packages)
# This is where Python looks on Debian/Ubuntu systems
echo "  Installing Python packages into dist-packages..."

# First install setuptools (provides distutils compatibility for Python 3.12+)
# Note: distutils was removed from Python 3.12 standard library
echo "  Installing setuptools (distutils compatibility)..."
"$PYTHON_BUNDLE/bin/pip" install --quiet --no-cache-dir --target="$PYTHON_BUNDLE/lib/python3.12/dist-packages" \
    setuptools 2>&1 | tail -3

echo "  Installing application packages..."
"$PYTHON_BUNDLE/bin/pip" install --quiet --no-cache-dir --target="$PYTHON_BUNDLE/lib/python3.12/dist-packages" \
    fastapi "uvicorn[standard]" websockets pycryptodome pyguitarpro \
    Pillow midiutil python-multipart requests 2>&1 | tail -5

# Also create a site-packages symlink for compatibility
ln -sf "$PYTHON_BUNDLE/lib/python3.12/dist-packages" "$PYTHON_BUNDLE/lib/python3.12/site-packages" 2>/dev/null || true

# Create __pycache__ directories to ensure they're writable
find "$PYTHON_BUNDLE/lib/python3.12" -type d -name "__pycache__" -exec chmod +w {} \; 2>/dev/null || true

echo "  Python runtime size: $(du -sh "$PYTHON_BUNDLE" | cut -f1)"
echo "=== Python bundle complete ==="
