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

# Locate the Mac-capable RsCli tool source from the slopsmith repo.
# Prefer an already-cloned slopsmith tree (SLOPSMITH_DIR, set by
# build-common.sh's clone_slopsmith / local dev); otherwise shallow-
# clone slopsmith just to obtain rscli/.
RSCLI_SRC=""
if [[ -n "${SLOPSMITH_DIR:-}" && -f "$SLOPSMITH_DIR/rscli/Program.fs" ]]; then
    RSCLI_SRC="$SLOPSMITH_DIR/rscli"
    echo "  RsCli source: $RSCLI_SRC (from SLOPSMITH_DIR)"
else
    echo "  Fetching RsCli source from slopsmith repo"
    git clone --quiet --depth 1 \
        https://github.com/byrongamatos/slopsmith.git "$TMP_DIR/slopsmith-src"
    RSCLI_SRC="$TMP_DIR/slopsmith-src/rscli"
fi

for f in Program.fs RsCli.fsproj; do
    if [[ ! -f "$RSCLI_SRC/$f" ]]; then
        echo "ERROR: $RSCLI_SRC/$f not found — cannot build a Mac-capable RsCli." >&2
        exit 1
    fi
done

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
