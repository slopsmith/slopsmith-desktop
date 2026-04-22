#!/bin/bash
# Build RsCli — the .NET tool for Rocksmith 2014 file operations (PSARC
# extraction etc.). Clone is pinned to a specific commit of
# Rocksmith2014.NET via .build-config.json for reproducibility.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
CONFIG="$PROJECT_DIR/.build-config.json"

RS2014_REPO=$(python3 "$SCRIPT_DIR/parse-build-config.py" "$CONFIG" .external.rs2014net.repo)
RS2014_COMMIT=$(python3 "$SCRIPT_DIR/parse-build-config.py" "$CONFIG" .external.rs2014net.commit)

# .NET RID for the host platform. uname -m returns `x86_64` on Intel
# Macs/Linux and `arm64`/`aarch64` on Apple Silicon/ARM Linux; .NET RIDs
# use `x64` / `arm64` so we map explicitly.
ARCH="$(uname -m)"
case "$ARCH" in
    x86_64) ARCH="x64" ;;
    aarch64) ARCH="arm64" ;;
    arm64) ARCH="arm64" ;;
    *)
        echo "ERROR: unsupported architecture: $ARCH" >&2
        exit 1
        ;;
esac

case "$(uname -s)" in
    Linux)  RID="linux-$ARCH" ;;
    Darwin) RID="osx-$ARCH" ;;
    MINGW*|MSYS*|CYGWIN*) RID="win-$ARCH" ;;
    *)
        echo "ERROR: unsupported OS: $(uname -s)" >&2
        exit 1
        ;;
esac

echo "=== Building RsCli for $RID ==="
echo "  Source: https://github.com/$RS2014_REPO @ $RS2014_COMMIT"

TMP_DIR=$(mktemp -d)
trap 'rm -rf "$TMP_DIR"' EXIT

git clone --quiet "https://github.com/$RS2014_REPO.git" "$TMP_DIR/rs2014net"
git -C "$TMP_DIR/rs2014net" checkout --quiet "$RS2014_COMMIT"

cd "$TMP_DIR/rs2014net/tools/RsCli"
echo "  Running dotnet publish"
dotnet publish -c Release -r "$RID" --self-contained -o "$TMP_DIR/rscli-out" 2>&1 | tail -3

mkdir -p "$PROJECT_DIR/resources/bin/rscli"
cp -r "$TMP_DIR/rscli-out/"* "$PROJECT_DIR/resources/bin/rscli/"

echo "  RsCli: $(du -sh "$PROJECT_DIR/resources/bin/rscli/" | cut -f1)"
echo "=== RsCli build complete ==="
