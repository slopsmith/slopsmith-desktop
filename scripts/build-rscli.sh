#!/bin/bash
# Build RsCli — the .NET tool for Rocksmith 2014 file operations (PSARC
# extraction, SNG<->XML conversion).
#
# The Rocksmith2014.NET *libraries* are pinned to a commit via
# .build-config.json for reproducibility. The RsCli *tool source*
# (Program.fs / RsCli.fsproj), however, is taken from the slopsmith
# repo — NOT from the pinned Rocksmith2014.NET tree.
#
# Why: slopsmith/rscli/Program.fs is the only copy with Mac-platform
# SNG support (`sng2xml <in> <out> [pc|mac]`). The copy carried in the
# Rocksmith2014.NET fork is PC-only and cannot decode Mac (`_m`) PSARCs
# — a desktop build using it reports "no arrangements" on any Mac CDLC.
# The slopsmith Dockerfile already overlays the slopsmith copy this way;
# this mirrors that so the desktop build's RsCli matches the Docker one.

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
echo "  Rocksmith2014.NET libs: https://github.com/$RS2014_REPO @ $RS2014_COMMIT"

TMP_DIR=$(mktemp -d)
trap 'rm -rf "$TMP_DIR"' EXIT

git clone --quiet "https://github.com/$RS2014_REPO.git" "$TMP_DIR/rs2014net"
git -C "$TMP_DIR/rs2014net" checkout --quiet "$RS2014_COMMIT"

# Locate the slopsmith checkout — the Mac-capable RsCli tool source
# lives in its rscli/ directory. Search order matches bundle-slopsmith.sh
# / bundle-python.sh: an explicit $SLOPSMITH_DIR is honoured verbatim so
# a typo or partial checkout is surfaced rather than masked; otherwise
# fall back to ../slopsmith then ~/Repositories/slopsmith. In a full
# desktop build, clone_slopsmith() in build-common.sh runs first and
# exports $SLOPSMITH_DIR — so RsCli is built from the exact same
# slopsmith checkout that gets bundled into the app.
if [[ -z "${SLOPSMITH_DIR:-}" ]]; then
    if [[ -d "$PROJECT_DIR/../slopsmith" ]]; then
        SLOPSMITH_DIR="$PROJECT_DIR/../slopsmith"
    elif [[ -d "$HOME/Repositories/slopsmith" ]]; then
        SLOPSMITH_DIR="$HOME/Repositories/slopsmith"
    fi
fi
RSCLI_SRC="${SLOPSMITH_DIR:-}/rscli"
if [[ -z "${SLOPSMITH_DIR:-}" ]] || [[ ! -f "$RSCLI_SRC/Program.fs" ]] || [[ ! -f "$RSCLI_SRC/RsCli.fsproj" ]]; then
    echo "ERROR: slopsmith rscli/ source not found — required to build a Mac-capable RsCli." >&2
    echo "Searched:" >&2
    echo "  \$SLOPSMITH_DIR=${SLOPSMITH_DIR:-<unset>}" >&2
    echo "  $PROJECT_DIR/../slopsmith" >&2
    echo "  $HOME/Repositories/slopsmith" >&2
    echo "Expected \$SLOPSMITH_DIR to be exported by clone_slopsmith() in build-common.sh," >&2
    echo "or clone slopsmith next to this repo: git clone https://github.com/byrongamatos/slopsmith.git $PROJECT_DIR/../slopsmith" >&2
    exit 1
fi
echo "  RsCli source: $RSCLI_SRC"

# Overlay slopsmith's tool source onto the Rocksmith2014.NET tree: the
# build links against the pinned libraries but compiles the Mac-capable
# CLI. Same approach as the slopsmith Dockerfile's `COPY rscli/...`.
mkdir -p "$TMP_DIR/rs2014net/tools/RsCli"
cp "$RSCLI_SRC/Program.fs" "$RSCLI_SRC/RsCli.fsproj" "$TMP_DIR/rs2014net/tools/RsCli/"

cd "$TMP_DIR/rs2014net/tools/RsCli"
echo "  Running dotnet publish"
dotnet publish -c Release -r "$RID" --self-contained -o "$TMP_DIR/rscli-out" 2>&1 | tail -3

# Guard: fail the build if the produced RsCli lacks Mac-platform support
# (e.g. the overlay silently didn't take). Without this a regression
# would ship a PC-only RsCli and Mac PSARCs would break for users. The
# binary is built for the host RID, so it runs on this build machine.
RSCLI_BIN=""
for cand in "$TMP_DIR/rscli-out/RsCli" "$TMP_DIR/rscli-out/RsCli.exe"; do
    [[ -f "$cand" ]] && RSCLI_BIN="$cand" && break
done
if [[ -z "$RSCLI_BIN" ]]; then
    echo "ERROR: dotnet publish did not produce an RsCli binary." >&2
    exit 1
fi
RSCLI_USAGE="$("$RSCLI_BIN" 2>&1 || true)"
if ! grep -qF '[pc|mac]' <<<"$RSCLI_USAGE"; then
    echo "ERROR: built RsCli lacks Mac-platform support — '[pc|mac]' missing from its" >&2
    echo "       usage output. The slopsmith/rscli overlay did not take effect." >&2
    exit 1
fi
echo "  RsCli sng2xml supports [pc|mac] platform selection"

mkdir -p "$PROJECT_DIR/resources/bin/rscli"
cp -r "$TMP_DIR/rscli-out/"* "$PROJECT_DIR/resources/bin/rscli/"

echo "  RsCli: $(du -sh "$PROJECT_DIR/resources/bin/rscli/" | cut -f1)"
echo "=== RsCli build complete ==="
