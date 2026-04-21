#!/bin/bash
# Build RsCli - .NET tool for Rocksmith 2014 file operations
# This is required for processing .psarc files

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

echo "=== Building RsCli ==="

# Determine platform RID
if [[ "$OSTYPE" == "msys" ]] || [[ "$OSTYPE" == "cygwin" ]]; then
    RID="win-x64"
elif [[ "$OSTYPE" == "darwin"* ]]; then
    RID="osx-$(uname -m)"
else
    RID="linux-x64"
fi

echo "  Platform: $RID"

# Clone and build
TMP_DIR=$(mktemp -d)
git clone --depth 1 https://github.com/byrongamatos/Rocksmith2014.NET.git "$TMP_DIR/rs2014net"
cd "$TMP_DIR/rs2014net/tools/RsCli"

echo "  Building with dotnet..."
dotnet publish -c Release -r "$RID" --self-contained -o "$TMP_DIR/rscli-out" 2>&1 | tail -3

# Copy to resources
mkdir -p "$PROJECT_DIR/resources/bin/rscli"
cp -r "$TMP_DIR/rscli-out/"* "$PROJECT_DIR/resources/bin/rscli/"

# Cleanup
cd "$PROJECT_DIR"
rm -rf "$TMP_DIR"

echo "  RsCli bundled for $RID ($(du -sh "$PROJECT_DIR/resources/bin/rscli/" | cut -f1))"
echo "=== RsCli build complete ==="
