#!/bin/bash
# Reproducible Linux AppImage build via Docker. Uses the DevContainer
# image (.devcontainer/Dockerfile) + the same npm scripts that CI runs,
# so local builds match GitHub Actions bit-for-bit modulo the base
# image's rolling security patches.
#
# The container is deliberately NOT auto-removed after the run so the
# full build tree + cache stays available for debugging. Clean up
# manually when done:
#   docker stop <name> && docker rm <name>
# The script prints the exact command on completion or failure.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
DEVCONTAINER_DIR="$PROJECT_DIR/.devcontainer"
SLOPSMITH_DIR="$PROJECT_DIR/../slopsmith"
BUILD_CONFIG="$PROJECT_DIR/.build-config.json"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

echo "=== Slopsmith Desktop reproducible build ==="
echo ""

# ── Read build configuration ─────────────────────────────────────────────────

if [ ! -f "$BUILD_CONFIG" ]; then
    echo -e "${RED}Error: $BUILD_CONFIG not found${NC}" >&2
    exit 1
fi

if ! python3 "$SCRIPT_DIR/parse-build-config.py" "$BUILD_CONFIG" >/dev/null; then
    echo -e "${RED}Error: $BUILD_CONFIG is not valid JSON${NC}" >&2
    exit 1
fi

get_cfg() { python3 "$SCRIPT_DIR/parse-build-config.py" "$BUILD_CONFIG" "$1"; }

echo -e "${BLUE}Build configuration:${NC}"
echo "  Node:     $(get_cfg .versions.node)"
echo "  Python:   $(get_cfg .versions.python)"
echo "  .NET:     $(get_cfg .versions.dotnet)"
echo "  Electron: $(get_cfg .versions.electron)"
echo "  Ubuntu:   $(get_cfg .versions.ubuntu)"
echo ""

# ── Prerequisites ────────────────────────────────────────────────────────────

if ! command -v docker &>/dev/null; then
    echo -e "${RED}Error: Docker is not installed${NC}" >&2
    echo "Install: https://docs.docker.com/get-docker/" >&2
    exit 1
fi

if ! docker info &>/dev/null; then
    echo -e "${RED}Error: Docker daemon is not running${NC}" >&2
    exit 1
fi

if [ ! -d "$SLOPSMITH_DIR" ]; then
    echo -e "${RED}Error: Slopsmith repository not found at $SLOPSMITH_DIR${NC}" >&2
    echo "" >&2
    echo "Clone it adjacent to this repository:" >&2
    echo "  git clone https://github.com/byrongamatos/slopsmith.git $(dirname "$PROJECT_DIR")/slopsmith" >&2
    exit 1
fi

echo -e "${GREEN}✓${NC} Slopsmith repository found at $SLOPSMITH_DIR"

# ── Build image ──────────────────────────────────────────────────────────────

echo ""
echo "Building container image…"
# Build from project root so COPY instructions can reach .build-config.json,
# .packages/, and scripts/.
docker build -f "$DEVCONTAINER_DIR/Dockerfile" -t slopsmith-desktop-builder "$PROJECT_DIR"

# ── Run build ────────────────────────────────────────────────────────────────

# Seconds + PID + $RANDOM so two invocations inside the same second don't
# collide on the docker --name conflict.
CONTAINER_NAME="slopsmith-build-$(date +%s)-$$-$RANDOM"
echo ""
echo -e "${BLUE}Container name:${NC} $CONTAINER_NAME"
echo "  (not auto-removed — attach a shell if the build fails)"
echo ""

set +e
docker run \
    --name "$CONTAINER_NAME" \
    -v "$PROJECT_DIR:/workspace" \
    -v "$SLOPSMITH_DIR:/workspaces/slopsmith" \
    -w /workspace \
    -e SLOPSMITH_DIR=/workspaces/slopsmith \
    -e ELECTRON_CACHE=/home/vscode/.cache/electron \
    -e ELECTRON_BUILDER_CACHE=/home/vscode/.cache/electron-builder \
    -e DEBUG=electron-builder \
    -t \
    slopsmith-desktop-builder \
    bash -c '
        set -e
        export PATH="/workspace/node_modules/.bin:$PATH"

        # npm install (not ci): package-lock.json is gitignored, so a bare
        # clone has no lockfile for ci to consume. CI uses install too.
        echo "=== Step 1/3: npm install ==="
        npm install

        echo "=== Step 2/3: git submodules ==="
        git submodule update --init --recursive

        # dist:linux chains build:native → bundle → build:ts → electron-builder,
        # so the native addon + RsCli end up in the AppImage.
        echo "=== Step 3/3: npm run dist:linux ==="
        NODE_ENV=production npm run dist:linux

        # Glob-safe existence check: compgen returns 0 iff at least one match.
        if compgen -G "release/Slopsmith-*.AppImage" >/dev/null; then
            ls -lh release/Slopsmith-*.AppImage
        else
            echo "ERROR: no AppImage found in release/" >&2
            ls -la release/ 2>/dev/null || echo "release/ does not exist" >&2
            exit 1
        fi
    '
BUILD_EXIT_CODE=$?
set -e

# ── Report ───────────────────────────────────────────────────────────────────

echo ""
if [ $BUILD_EXIT_CODE -eq 0 ]; then
    echo -e "${GREEN}✓ Build succeeded${NC}"
    echo ""
    if compgen -G "$PROJECT_DIR/release/*.AppImage" >/dev/null || compgen -G "$PROJECT_DIR/release/*.deb" >/dev/null; then
        ls -lh "$PROJECT_DIR/release/" | awk 'NR==1 || /AppImage|\.deb/'
    fi
    echo ""
    echo "  Container preserved: docker exec -it $CONTAINER_NAME /bin/bash"
    echo "  Clean up:            docker stop $CONTAINER_NAME && docker rm $CONTAINER_NAME"
else
    echo -e "${RED}✗ Build failed (exit $BUILD_EXIT_CODE)${NC}"
    echo ""
    echo "  Last 100 lines of container output:"
    echo "  -----------------------------------"
    docker logs --tail 100 "$CONTAINER_NAME" 2>&1 || echo "  (logs unavailable)"
    echo "  -----------------------------------"
    echo ""
    echo "  Debug:"
    echo "    docker exec -it $CONTAINER_NAME /bin/bash"
    echo "    docker logs $CONTAINER_NAME"
    echo "    docker stop $CONTAINER_NAME && docker rm $CONTAINER_NAME  # when done"
fi

# ── Image housekeeping ───────────────────────────────────────────────────────
# Opt-in because `docker image prune --filter dangling=true` applies to the
# whole Docker host, not just this project. Setting
# SLOPSMITH_PRUNE_DANGLING_IMAGES=1 re-enables cleanup if you want it.
echo ""
if [ "${SLOPSMITH_PRUNE_DANGLING_IMAGES:-0}" = "1" ]; then
    echo "Cleaning up dangling images (SLOPSMITH_PRUNE_DANGLING_IMAGES=1)…"
    docker image prune -f --filter "dangling=true" >/dev/null 2>&1 || true
else
    echo "Skipping dangling-image cleanup (set SLOPSMITH_PRUNE_DANGLING_IMAGES=1 to enable)."
fi

echo ""
echo "=== Done ==="
exit $BUILD_EXIT_CODE
