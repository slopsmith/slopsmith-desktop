#!/bin/bash
# Common build logic for all platforms
# Platform scripts source this and implement three functions:
#   install_system_deps() - install OS packages
#   bundle_python_impl() - bundle Python runtime
#   bundle_binaries_impl() - bundle system binaries
#   get_expected_artifacts() - return globs used to verify binaries are present

set -euo pipefail

# Check if this is being sourced by a platform script
if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
    echo "Error: build-common.sh should not be run directly" >&2
    echo "Run ./build-release.sh instead" >&2
    exit 1
fi

# Script directory must be set by sourcing script
if [[ -z "${SCRIPT_DIR:-}" ]]; then
    echo "Error: SCRIPT_DIR not set by sourcing script" >&2
    exit 1
fi

# Colors
if [[ -z "${RED:-}" ]]; then
    RED='\033[0;31m'
    GREEN='\033[0;32m'
    YELLOW='\033[1;33m'
    BLUE='\033[0;34m'
    NC='\033[0m'
fi

# Check that required variables are set
if [[ -z "${PROJECT_DIR:-}" ]]; then
    echo "Error: PROJECT_DIR not set by sourcing script" >&2
    exit 1
fi

if [[ -z "${PLATFORM:-}" ]]; then
    echo "Error: PLATFORM not set by sourcing script" >&2
    exit 1
fi

# Ensure platform is lowercase
PLATFORM="$(echo "$PLATFORM" | tr '[:upper:]' '[:lower:]')"

# Validate platform
if [[ ! "$PLATFORM" =~ ^(linux|macos|windows)$ ]]; then
    echo -e "${RED}Error: Invalid platform: $PLATFORM${NC}" >&2
    exit 1
fi

# Map platform to npm dist script name
# PLATFORM="linux" → "dist:linux"
# PLATFORM="macos" → "dist:mac"
# PLATFORM="windows" → "dist:win"
case "$PLATFORM" in
    linux)
        NPM_DIST="dist:linux"
        ;;
    macos)
        NPM_DIST="dist:mac"
        ;;
    windows)
        NPM_DIST="dist:win"
        ;;
esac

# Configuration file
CONFIG="$PROJECT_DIR/.build-config.json"
PARSE_CONFIG="$SCRIPT_DIR/parse-build-config.py"

# --- Shared Python packages (bundled for all platforms) ---
# These are installed into the bundled Python runtime for server.py
PYTHON_PACKAGES="fastapi uvicorn[standard] websockets pycryptodome pyguitarpro Pillow midiutil python-multipart requests psarc"

# Check config file
if [[ ! -f "$CONFIG" ]]; then
    echo -e "${RED}Error: $CONFIG not found${NC}" >&2
    exit 1
fi

if ! python3 "$PARSE_CONFIG" "$CONFIG" >/dev/null; then
    echo -e "${RED}Error: $CONFIG is not valid JSON${NC}" >&2
    exit 1
fi

get_cfg() { python3 "$PARSE_CONFIG" "$CONFIG" "$1"; }

# --- Platform functions (to be implemented by platform scripts) ---
# Platform scripts MUST implement these three functions:
# install_system_deps()
# bundle_python_impl()
# bundle_binaries_impl()

# --- Common Build Steps ---

# Clone Slopsmith and plugins (shared across all platforms)
clone_slopsmith() {
	local clone_dir="${1:-${RUNNER_TEMP:-/tmp}/slopsmith}"

	# Skip if already set for local development
	if [[ -n "${SLOPSMITH_DIR:-}" ]] && [[ -d "$SLOPSMITH_DIR" ]]; then
		echo "Using existing SLOPSMITH_DIR: $SLOPSMITH_DIR"
		return 0
	fi

	echo "Cloning Slopsmith repository..."
	git clone --depth 1 https://github.com/byrongamatos/slopsmith.git "$clone_dir"

	# Remove broken symlinks from plugins dir
	find "$clone_dir/plugins" -maxdepth 1 -type l -delete 2>/dev/null || true

	# Clone essential plugins
	cd "$clone_dir/plugins"
	for repo in 3dhighway backingtrack cf fretboard metronome midi notedetect practice profileimport sectionmap setlist tabimport tabview tones ug; do
		git clone --depth 1 "https://github.com/byrongamatos/slopsmith-plugin-${repo}.git" "${repo}" 2>/dev/null || echo " skipped ${repo}"
	done

	export SLOPSMITH_DIR="$clone_dir"
	echo "Cloned $(ls -d */ 2>/dev/null | wc -l) plugins"
	cd - >/dev/null
}


step=1

echo_validate_env() {
    echo -e "${BLUE}Step $step: Validating environment${NC}"
    step=$((step + 1))
}

echo_step() {
    echo -e "${BLUE}Step $step: $1${NC}"
    step=$((step + 1))
}

echo_summary() {
    echo -e "${GREEN}✓${NC} $1"
}

echo_warning() {
    echo -e "${YELLOW}!${NC} $1"
}

echo_error() {
    echo -e "${RED}✗${NC} $1"
}

validate_environment() {
    echo_validate_env

    NODE_VERSION=$(get_cfg .versions.node)
    PYTHON_VERSION=$(get_cfg .versions.python)
    DOTNET_VERSION=$(get_cfg .versions.dotnet)

    echo "Platform: $PLATFORM"
    echo "Node: $NODE_VERSION"
    echo "Python: $PYTHON_VERSION"
    echo ".NET: $DOTNET_VERSION"
    echo ""

    # Check Node.js
    if command -v node &>/dev/null; then
        INSTALLED_NODE=$(node -p "process.version.replace('v', '')")
        echo_summary "Found Node.js $INSTALLED_NODE"
    else
        echo_error "Node.js not found"
        exit 1
    fi

    # Check Python 3
    if command -v python3 &>/dev/null; then
        INSTALLED_PYTHON=$(python3 -c "import sys; print(f'{sys.version_info.major}.{sys.version_info.minor}')")
        echo_summary "Found Python $INSTALLED_PYTHON"
    else
        echo_error "Python 3 not found"
        exit 1
    fi

    # Check .NET
    if command -v dotnet &>/dev/null; then
        INSTALLED_DOTNET=$(dotnet --version)
        echo_summary "Found .NET $INSTALLED_DOTNET"
    else
        echo_error ".NET SDK not found"
        exit 1
    fi

    echo ""
}

install_npm_deps() {
    echo_step "Installing npm dependencies"
    npm install
    echo_summary "npm dependencies installed"
    echo ""
}

build_native_addons() {
    echo_step "Building native addons (audio engine + RsCli)"
    npm run build:native
    echo_summary "Native addons built"
    echo ""
}

bundle_slopsmith() {
    echo_step "Bundling Slopsmith and plugins"
    npm run bundle:slopsmith
    echo_summary "Slopsmith bundled"
    echo ""
}

bundle_python() {
    mkdir -p "$PROJECT_DIR/resources"
    bundle_python_impl
    echo_summary "Python runtime bundled"
    echo ""
}

bundle_binaries() {
    mkdir -p "$PROJECT_DIR/resources/bin"
    bundle_binaries_impl
    echo_summary "System binaries bundled"
    echo ""
}

verify_bundled_binaries() {
  # Smoke test: verify bundled binaries are executable and can run
  local bin_dir="$PROJECT_DIR/resources/bin"
  local ext=""
  if [[ "$PLATFORM" == "windows" ]]; then
    ext=".exe"
  fi
  
  echo_step "Verifying bundled binaries"
  
  for binary in fluidsynth ffmpeg vgmstream-cli; do
    local path="$bin_dir/${binary}${ext}"
    if [[ ! -x "$path" && ! -f "$path" ]]; then
      echo_error "Missing bundled binary: $path"
      exit 1
    fi
    # Try to run --version or --help (some binaries only support one)
    if ! "$path" --version >/dev/null 2>&1 && ! "$path" --help >/dev/null 2>&1; then
      echo_error "Binary $binary failed to execute"
      exit 1
    fi
    echo "  ✓ $binary"
  done
  
  echo_summary "All bundled binaries verified"
  echo ""
}

bundle_soundfont() {
    echo_step "Bundling default soundfont"
    bash "$SCRIPT_DIR/bundle-soundfont.sh"
    echo_summary "Soundfont bundled"
    echo ""
}

build_typescript() {
    echo_step "Building TypeScript"
    npm run build:ts
    echo_summary "TypeScript built"
    echo ""
}

package_application() {
    echo_step "Packaging application"
    npm run "$NPM_DIST"
    echo_summary "Application packaged"
    echo ""
}

verify_artifacts() {
    echo_step "Verifying artifacts"

    ARTIFACTS_FOUND=0

    # Read patterns into array (avoid process substitution for CI compatibility)
  patterns=()
  tempfile=$(mktemp)
  get_expected_artifacts > "$tempfile"
  cat "$tempfile" >&2
  while IFS= read -r line; do
    patterns+=("$line")
  done < "$tempfile"
  rm -f "$tempfile"
  for pattern in "${patterns[@]}"; do
    shopt -s nullglob
    files=($pattern)
    shopt -u nullglob
    if [ ${#files[@]} -gt 0 ]; then
      ARTIFACTS_FOUND=1
      break
    fi
  done

    if [[ $ARTIFACTS_FOUND -eq 1 ]]; then
        echo_summary "Build successful!"
        echo ""
        ls -lh "$PROJECT_DIR/release/" 2>/dev/null | grep -v "^total" | awk 'NR > 1' | head -10 || true
    else
        echo_error "No artifacts found"
        if [[ -d "$PROJECT_DIR/release" ]]; then
            echo "Contents of release/:"
            ls -la "$PROJECT_DIR/release/" 2>&1 || echo "(directory empty)"
        else
            echo "release/ directory doesn't exist"
        fi
        exit 1
    fi
    echo ""
}

# Main entry point - platform scripts call this
main() {
    local start_time=$(date +%s)

    case "$PLATFORM" in
        linux|macos|windows)
            ;;
        *)
            echo_error "Unsupported platform: $PLATFORM"
            exit 1
            ;;
    esac

  # Verify that all required functions are defined by the sourcing platform script
  local missing_functions=()
  local required_funcs=(
    install_system_deps
    bundle_python_impl
    bundle_binaries_impl
    get_expected_artifacts
  )

  for func in "${required_funcs[@]}"; do
    if ! type "$func" &>/dev/null; then
      missing_functions+=("$func")
    fi
  done

  if [[ ${#missing_functions[@]} -gt 0 ]]; then
    echo_error "Required functions not defined by platform script:"
    for func in "${missing_functions[@]}"; do
      echo "  - $func"
    done
    exit 1
  fi

validate_environment
install_system_deps
install_npm_deps
build_native_addons
clone_slopsmith
bundle_slopsmith
bundle_python
bundle_binaries
verify_bundled_binaries
bundle_soundfont
build_typescript
package_application
verify_artifacts

    local end_time=$(date +%s)
    local duration=$((end_time - start_time))

    echo -e "${GREEN}✓${NC} Build complete for $PLATFORM in ${duration}s"
    echo "Output: $PROJECT_DIR/release/"
}
