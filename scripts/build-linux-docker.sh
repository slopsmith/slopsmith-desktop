#!/bin/bash
# Docker-based Linux build wrapper
# Runs build-linux-ubuntu.sh inside a reproducible container

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
DEVCONTAINER_DIR="$PROJECT_DIR/.devcontainer"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

echo "=== Slopsmith Desktop Docker Build ==="
echo ""
echo "This script provides reproducible Linux builds by running"
echo "build-linux-ubuntu.sh inside a Docker container."
echo ""

# Check prerequisites
echo -e "${BLUE}Checking prerequisites...${NC}"

if ! command -v docker &>/dev/null; then
    echo -e "${RED}Error: Docker is not installed${NC}" >&2
    echo "Install: https://docs.docker.com/get-docker/" >&2
    exit 1
fi

if ! docker info &>/dev/null; then
    echo -e "${RED}Error: Docker daemon is not running${NC}" >&2
    exit 1
fi

echo -e "${GREEN}✓${NC} Docker available"
echo ""

# Build container image
echo -e "${BLUE}Building container image...${NC}"
echo " (This will take a few minutes on first run)"
echo ""

docker build \
    -f "$DEVCONTAINER_DIR/Dockerfile" \
    -t slopsmith-ubuntu-builder \
    "$PROJECT_DIR"
# `set -e` at the top of this script already aborts on a failed
# `docker build` — no manual `$?` check needed (and the check that
# was here would in practice be unreachable).

echo -e "${GREEN}✓${NC} Container image built"
echo ""

# Clear stale CMake build cache. CMakeCache.txt bakes in the build path;
# when the project is mounted at a different path inside the container the
# paths don't match and cmake aborts. A clean build/ guarantees consistency.
if [[ -d "$PROJECT_DIR/build" ]]; then
    echo -e "${BLUE}Clearing stale CMake cache...${NC}"
    rm -rf "$PROJECT_DIR/build"
fi

# Generate unique container name
CONTAINER_NAME="slopsmith-build-$(date +%s)-$$-$RANDOM"

echo -e "${BLUE}Running build in container...${NC}"
echo -e "${BLUE}Container name:${NC} $CONTAINER_NAME"
echo ""
echo "The container will be preserved after the build to allow debugging."
echo "Clean up when done:"
echo "  docker stop $CONTAINER_NAME && docker rm $CONTAINER_NAME"
echo ""

set +e
docker run \
    --name "$CONTAINER_NAME" \
    -v "$PROJECT_DIR:/workspace" \
    -w /workspace \
    -e ELECTRON_CACHE=/home/vscode/.cache/electron \
    -e ELECTRON_BUILDER_CACHE=/home/vscode/.cache/electron-builder \
    -e GIT_TERMINAL_PROMPT=0 \
    -t \
    slopsmith-ubuntu-builder \
    bash -c './scripts/build-linux-ubuntu.sh'
BUILD_EXIT_CODE=$?
set -e

echo ""
if [[ $BUILD_EXIT_CODE -eq 0 ]]; then
    echo -e "${GREEN}✓${NC} Build completed successfully!"
else
    echo -e "${RED}✗${NC} Build failed (exit code: $BUILD_EXIT_CODE)"
    echo ""
    echo "To debug:"
    echo "  docker exec -it $CONTAINER_NAME /bin/bash"
    echo "  docker logs $CONTAINER_NAME"
    echo ""
fi

exit $BUILD_EXIT_CODE
