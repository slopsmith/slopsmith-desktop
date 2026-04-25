#!/bin/bash
# Docker-based Linux build wrapper
# Runs build-linux-ubuntu.sh inside a reproducible container

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
SLOPSMITH_DIR="$PROJECT_DIR/../slopsmith"
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

echo -e "${GREEN}✓${NC} Docker and Slopsmith repository found"
echo ""

# Build container image
echo -e "${BLUE}Building container image...${NC}"
echo " (This will take a few minutes on first run)"
echo ""

docker build \
    -f "$DEVCONTAINER_DIR/Dockerfile" \
    -t slopsmith-ubuntu-builder \
    "$PROJECT_DIR"

if [[ $? -ne 0 ]]; then
    echo -e "${RED}Error: Docker build failed${NC}" >&2
    exit 1
fi

echo -e "${GREEN}✓${NC} Container image built"
echo ""

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
    -v "$SLOPSMITH_DIR:/workspaces/slopsmith" \
    -w /workspace \
    -e SLOPSMITH_DIR=/workspaces/slopsmith \
    -e ELECTRON_CACHE=/home/vscode/.cache/electron \
    -e ELECTRON_BUILDER_CACHE=/home/vscode/.cache/electron-builder \
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
