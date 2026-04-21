#!/bin/bash
# Standalone build script for Slopsmith Desktop
# Uses Docker to create reproducible builds matching CI
#
# This script delegates to npm scripts which are the single source of truth
# for the build process. See package.json for the actual build steps.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
DEVCONTAINER_DIR="$PROJECT_DIR/.devcontainer"
SLOPSMITH_DIR="$PROJECT_DIR/../slopsmith"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo "=== Slopsmith Desktop Reproducible Build ==="
echo ""

# ── Read Build Configuration ─────────────────────────────────────────────────

BUILD_CONFIG="$PROJECT_DIR/.build-config.json"
if [ ! -f "$BUILD_CONFIG" ]; then
    echo -e "${RED}Error: Build configuration not found: $BUILD_CONFIG${NC}"
    exit 1
fi

# Validate JSON
if ! jq empty "$BUILD_CONFIG" 2>/dev/null; then
    echo -e "${RED}Error: $BUILD_CONFIG is not valid JSON${NC}"
    exit 1
fi

echo -e "${BLUE}Build Configuration:${NC}"
echo "  Node: $(jq -r '.versions.node' "$BUILD_CONFIG")"
echo "  Python: $(jq -r '.versions.python' "$BUILD_CONFIG")"
echo "  .NET: $(jq -r '.versions.dotnet' "$BUILD_CONFIG")"
echo "  Electron: $(jq -r '.versions.electron' "$BUILD_CONFIG")"
echo "  Ubuntu: $(jq -r '.versions.ubuntu' "$BUILD_CONFIG")"
echo ""

# ── Check Prerequisites ─────────────────────────────────────────────────────

if ! command -v docker &> /dev/null; then
    echo -e "${RED}Error: Docker is not installed${NC}"
    echo "Please install Docker: https://docs.docker.com/get-docker/"
    exit 1
fi

if ! docker info &> /dev/null; then
    echo -e "${RED}Error: Docker daemon is not running${NC}"
    echo "Please start Docker and try again"
    exit 1
fi

# ── Check Slopsmith Repository ─────────────────────────────────────────────

if [ ! -d "$SLOPSMITH_DIR" ]; then
    echo -e "${RED}Error: Slopsmith repository not found at expected location${NC}"
    echo ""
    echo "This build requires the Slopsmith server repository."
    echo "Please clone it adjacent to this repository:"
    echo ""
    echo "  git clone https://github.com/byrongamatos/slopsmith.git \\"
    echo "    $(dirname "$PROJECT_DIR")/slopsmith"
    echo ""
    echo "Expected location: $SLOPSMITH_DIR"
    exit 1
fi

echo -e "${GREEN}✓${NC} Slopsmith repository found"
echo "  Location: $SLOPSMITH_DIR"

# ── Build Docker Image ────────────────────────────────────────────────────

echo ""
echo "Building container image..."
echo "  (Note: If you see a deprecation warning about the legacy builder, you can ignore it)"
echo "  (To use BuildKit: https://docs.docker.com/go/buildx/)"
echo ""
# Build from project root so Dockerfile can access .build-config.json and .packages/
docker build -f "$DEVCONTAINER_DIR/Dockerfile" -t slopsmith-desktop-builder "$PROJECT_DIR"

# ── Run Build ───────────────────────────────────────────────────────────────

echo ""
echo "Running build in container..."
echo "  This will create AppImage and .deb packages"
echo "  Output will be in: $PROJECT_DIR/release/"
echo ""
echo "  Build steps (from package.json):"
echo "    1. build:native (audio engine + RsCli)"
echo "    2. bundle (Slopsmith + Python + binaries)"
echo "    3. build:ts (TypeScript compilation)"
echo "    4. electron-builder (package creation)"
echo ""

# Run the build using npm scripts - this is the single source of truth
# Note: --rm is removed so container persists for debugging if build fails
CONTAINER_NAME="slopsmith-build-$(date +%s)"
echo "  Container name: $CONTAINER_NAME"
echo ""

docker run \
    --name "$CONTAINER_NAME" \
    -v "$PROJECT_DIR:/workspace" \
    -v "$SLOPSMITH_DIR:/workspaces/slopsmith" \
    -w /workspace \
    -e SLOPSMITH_DIR=/workspaces/slopsmith \
    -e ELECTRON_CACHE=/home/vscode/.cache/electron \
    -e ELECTRON_BUILDER_CACHE=/home/vscode/.cache/electron-builder \
    -e DEBUG=electron-builder \
    -e NODE_OPTIONS=--openssl-legacy-provider \
    -t \
    slopsmith-desktop-builder \
    bash -c '
        set -e
        # Ensure node_modules/.bin is in PATH for npm scripts
        export PATH="/workspace/node_modules/.bin:$PATH"
        
        # Clean install of dependencies (runs postinstall which compiles native modules)
        # Note: NODE_ENV is not set to production here so devDependencies are installed
        echo "=== Step 1/5: Installing npm dependencies ==="
        npm ci || { echo "ERROR: npm ci failed"; exit 1; }
        
        echo "=== Step 2/5: Initializing git submodules ==="
        git submodule update --init --recursive || { echo "ERROR: git submodule update failed"; exit 1; }
        
        # Note: We do NOT clean the build directory here.
        # The native addon is built during postinstall, and dist:linux runs build:native
        # which will rebuild it anyway. Cleaning it would be redundant.
        
        echo "=== Step 3/5: Building application ==="
        NODE_ENV=production npm run dist:linux || { echo "ERROR: npm run dist:linux failed"; exit 1; }
        
        echo "=== Step 4/5: Verifying build output ==="
        if [ ! -f release/Slopsmith-*.AppImage ]; then
            echo "ERROR: No AppImage found in release/ directory"
            ls -la release/ || echo "release directory does not exist"
            exit 1
        fi
        
        echo "=== Step 5/5: Build completed successfully ==="
        ls -lh release/Slopsmith-*.AppImage
    '

BUILD_EXIT_CODE=$?

# If build failed, show the container logs
if [ $BUILD_EXIT_CODE -ne 0 ]; then
    echo ""
    echo -e "${RED}✗${NC} Build failed with exit code $BUILD_EXIT_CODE"
    echo ""
    echo "Last 100 lines of container output:"
    echo "======================================"
    docker logs --tail 100 "$CONTAINER_NAME" 2>&1 || echo "Could not retrieve logs"
    echo "======================================"
fi

# ── Verify Output ─────────────────────────────────────────────────────────

echo ""
if [ $BUILD_EXIT_CODE -eq 0 ]; then
    echo -e "${GREEN}✓${NC} Build completed successfully!"
    echo ""
    if [ -d "$PROJECT_DIR/release" ] && [ "$(ls -A "$PROJECT_DIR/release")" ]; then
        echo -e "${GREEN}✓${NC} Build artifacts created:"
        ls -lh "$PROJECT_DIR/release/" | grep -E "(AppImage|\.deb)" || true
        echo ""
        echo "Output location: $PROJECT_DIR/release/"
        echo ""
        echo -e "${BLUE}Container preserved for debugging:${NC}"
        echo "  Container name: $CONTAINER_NAME"
        echo ""
        echo "  Access Python environment for testing:"
        echo "    docker exec -it $CONTAINER_NAME /bin/bash"
        echo "    # Inside container:"
        echo "    /workspace/resources/python/runtime/bin/python3 -c \"import uvicorn; print('uvicorn:', uvicorn.__version__)\""
        echo ""
        echo "  Clean up when done:"
        echo "    docker stop $CONTAINER_NAME && docker rm $CONTAINER_NAME"
        echo ""
        echo "  The container contains the full build environment including:"
        echo "    - Python 3.12 with all installed packages"
        echo "    - Build artifacts in /workspace/release/"
        echo "    - Source code in /workspace/"
    else
        echo -e "${YELLOW}⚠${NC} No build artifacts found in $PROJECT_DIR/release/"
        echo "  Container $CONTAINER_NAME preserved for debugging"
    fi
else
    echo -e "${RED}✗${NC} Build failed with exit code $BUILD_EXIT_CODE"
    echo ""
    echo -e "${BLUE}Debugging options:${NC}"
    echo "  1. Re-run the build step inside container:"
    echo "       docker exec $CONTAINER_NAME bash -c 'npm run dist:linux'"
    echo ""
    echo "  2. Open a shell inside container:"
    echo "       docker exec -it $CONTAINER_NAME /bin/bash"
    echo "       # Then try: npm run dist:linux"
    echo ""
    echo "  3. View container logs:"
    echo "       docker logs $CONTAINER_NAME"
    echo ""
    echo "  4. Clean up when done:"
    echo "       docker stop $CONTAINER_NAME && docker rm $CONTAINER_NAME"
    echo ""
fi

# ── Cleanup Old Images ───────────────────────────────────────────────────
# Remove dangling images and old slopsmith-desktop-builder versions
# to prevent disk space bloat
echo ""
echo "Cleaning up old Docker images..."
docker image prune -f --filter "dangling=true" 2>/dev/null || true
docker images --format "table {{.Repository}}\t{{.Tag}}\t{{.ID}}\t{{.CreatedAt}}" | \
    grep "slopsmith-desktop-builder" | \
    awk 'NR>1 {print $3}' | \
    xargs -r docker rmi 2>/dev/null || true

echo ""
echo "=== Done ==="
