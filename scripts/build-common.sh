#!/bin/bash
# Common build logic for all platforms.
# Platform scripts source this file and implement four functions:
#   install_system_deps()       — install OS packages (apt / brew / winget)
#   bundle_python_impl()        — bundle Python runtime
#   bundle_binaries_impl()      — bundle system binaries (ffmpeg etc.)
#   get_expected_artifacts()    — globs verify_artifacts checks at the end

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

# is_skipped_lib() — glibc/loader skip list, shared verbatim with
# bundle-binaries.sh so the bundler and the audit never disagree.
source "$SCRIPT_DIR/bundled-lib-skiplist.sh"

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

# Configuration file
CONFIG="$PROJECT_DIR/.build-config.json"
PARSE_CONFIG="$SCRIPT_DIR/parse-build-config.py"

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
	# RUNNER_TEMP on Windows runners is a native Windows path
	# (e.g. `D:\a\_temp`) that Git Bash / MSYS tools don't reliably
	# treat as a filesystem path. POSIX `/tmp/slopsmith` is the
	# default; if a non-POSIX environment really wants RUNNER_TEMP, it
	# can pass an explicit clone_dir argument (resolved via cygpath -u
	# on Windows if needed).
	local clone_dir="${1:-/tmp/slopsmith}"

	# Skip if already set for local development
	if [[ -n "${SLOPSMITH_DIR:-}" ]] && [[ -d "$SLOPSMITH_DIR" ]]; then
		echo "Using existing SLOPSMITH_DIR: $SLOPSMITH_DIR"
		return 0
	fi

	# Make the clone re-runnable: a leftover dir from a previous failed
	# build would otherwise abort `git clone`. CI runners start fresh so
	# this is purely a quality-of-life fix for local re-runs.
	if [[ -d "$clone_dir" ]]; then
		rm -rf "$clone_dir"
	fi

	echo "Cloning Slopsmith repository..."
	git clone --depth 1 https://github.com/byrongamatos/slopsmith.git "$clone_dir"

	# Remove broken symlinks from plugins dir
	find "$clone_dir/plugins" -maxdepth 1 -type l -delete 2>/dev/null || true

	# Clone bundled plugins. Format per entry:
	#   <owner>/<repo>[@<branch>][:<dirname>]
	# Dirname defaults to <repo> minus the "slopsmith-plugin-" prefix
	# with hyphens replaced by underscores (slopsmith treats plugin
	# directories as Python module names, which can't contain dashes).
	# Provide an explicit dirname after a colon for repos that don't
	# follow the slopsmith-plugin-* naming convention. An optional
	# @<branch> clones a non-default branch (used to ship in-review
	# plugin work in a feature-branch test build).
	cd "$clone_dir/plugins"
	local plugins=(
		# byrongamatos plugins
		byrongamatos/slopsmith-plugin-drums
		byrongamatos/slopsmith-plugin-editor
		byrongamatos/slopsmith-plugin-fretboard
		byrongamatos/slopsmith-plugin-lyrics-karaoke
		byrongamatos/slopsmith-plugin-metronome
		byrongamatos/slopsmith-plugin-midi
		byrongamatos/slopsmith-plugin-multiplayer
		byrongamatos/slopsmith-plugin-nam-tone
		byrongamatos/slopsmith-plugin-notedetect
		byrongamatos/slopsmith-plugin-piano
		byrongamatos/slopsmith-plugin-practice
		byrongamatos/slopsmith-plugin-profileimport
		byrongamatos/slopsmith-plugin-sectionmap
		byrongamatos/slopsmith-plugin-setlist
		byrongamatos/slopsmith-plugin-stepmode
		byrongamatos/slopsmith-plugin-studio
		byrongamatos/slopsmith-plugin-tabimport
		byrongamatos/slopsmith-plugin-tabview
		byrongamatos/slopsmith-plugin-tones
		# Community plugins
		alleexx/slopsmith-plugin-transpose-chords
		masc0t/slopsmith-plugin-find-more
		masc0t/slopsmith-plugin-invert-highway
		masc0t/slopsmith-plugin-the-daily
		masc0t/slopsmith-plugin-themes
		masc0t/slopsmith-update-manager:update_manager
		narvasus/slopsmith-plugin-stem-mixer
		renanboni/slopsmith-plugin-jumpingtab
		topkoa/slopsmith-plugin-guitar-theory
		topkoa/slopsmith-plugin-sloppak-converter
		topkoa/slopsmith-plugin-splitscreen
		topkoa/slopsmith-plugin-stems
	)

	local total=0
	local cloned=0
	for entry in "${plugins[@]}"; do
		total=$((total + 1))
		# Split off an optional ":<dirname>" then an optional "@<branch>".
		# Git branch names can't contain ':' so the dirname split is safe
		# to do first; what's left is "<owner>/<repo>" or "<owner>/<repo>@<branch>".
		local spec="$entry" dirname="" branch=""
		if [[ "$spec" == *:* ]]; then
			dirname="${spec##*:}"
			spec="${spec%%:*}"
		fi
		local owner_repo="$spec"
		if [[ "$spec" == *@* ]]; then
			branch="${spec##*@}"
			owner_repo="${spec%%@*}"
		fi
		if [[ -z "$dirname" ]]; then
			dirname="${owner_repo##*/}"
			dirname="${dirname#slopsmith-plugin-}"
			dirname="${dirname//-/_}"
		fi
		local clone_args=(--depth 1)
		[[ -n "$branch" ]] && clone_args+=(--branch "$branch")
		if git clone "${clone_args[@]}" "https://github.com/${owner_repo}.git" "$dirname" 2>/dev/null; then
			cloned=$((cloned + 1))
		else
			echo " skipped ${owner_repo}${branch:+@$branch}"
		fi
	done

	export SLOPSMITH_DIR="$clone_dir"
	echo "Cloned ${cloned} of ${total} plugins"
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
  
  # Verify fluidsynth: supports --version
  local fs_path="$bin_dir/fluidsynth${ext}"
  if [[ ! -f "$fs_path" ]]; then
    echo_error "Missing bundled binary: $fs_path"
    exit 1
  fi
  if ! "$fs_path" --version >/dev/null 2>&1; then
    echo_error "Binary fluidsynth failed to execute"
    exit 1
  fi
  echo "  ✓ fluidsynth"
  
  # Verify ffmpeg: supports -version
  local ff_path="$bin_dir/ffmpeg${ext}"
  if [[ ! -f "$ff_path" ]]; then
    echo_error "Missing bundled binary: $ff_path"
    exit 1
  fi
  if ! "$ff_path" -version >/dev/null 2>&1; then
    echo_error "Binary ffmpeg failed to execute"
    exit 1
  fi
  echo "  ✓ ffmpeg"

  # Verify ffprobe: demucs spawns it before ffmpeg to read stream
  # metadata. Required on every platform because falling through to a
  # host-installed ffprobe makes stem splitting work on the build host
  # and silently fail on user machines without it.
  local ffp_path="$bin_dir/ffprobe${ext}"
  if [[ ! -f "$ffp_path" ]]; then
    echo_error "Missing bundled binary: $ffp_path"
    exit 1
  fi
  if ! "$ffp_path" -version >/dev/null 2>&1; then
    echo_error "Binary ffprobe failed to execute"
    exit 1
  fi
  echo "  ✓ ffprobe"
  
  # Verify vgmstream-cli: doesn't have --version, check it produces output with version
  local vgm_path="$bin_dir/vgmstream-cli${ext}"
  echo "  Checking vgmstream-cli at: $vgm_path"
  if [[ ! -f "$vgm_path" ]]; then
    echo_error "Missing bundled binary: $vgm_path"
    exit 1
  fi
  echo "  File exists, checking permissions:"
  ls -la "$vgm_path"
  # `file` is a diagnostic-only call — keep it best-effort so a minimal
  # base image (e.g. the Linux Docker builder, which doesn't ship the
  # libmagic-backed `file` binary) doesn't fail the build over a debug
  # line. The actual smoke test is the run-and-grep below.
  if command -v file >/dev/null 2>&1; then
    echo "  File type:"
    file "$vgm_path"
  fi
  echo "  Attempting to run vgmstream-cli..."
  local vgm_output
  local vgm_exit_code
  # vgmstream-cli with no args prints its version header then exits 1.
  # Capture the exit code via the if-branch so `set -e` doesn't trip
  # AND vgm_exit_code reflects the binary's real status (not the `|| true`
  # short-circuit that the previous form ended up reporting).
  if vgm_output=$("$vgm_path" 2>&1); then
    vgm_exit_code=0
  else
    vgm_exit_code=$?
  fi
  echo "  Exit code: $vgm_exit_code"
  echo "  Raw output:"
  echo "$vgm_output" | head -20
  echo "  Checking if output matches expected pattern..."
  if [[ -z "$vgm_output" ]]; then
    echo_error "Binary vgmstream-cli produced no output"
    exit 1
  fi
  if [[ ! "$vgm_output" =~ vgmstream.*CLI.*decoder ]]; then
    echo_error "Binary vgmstream-cli produced unexpected output"
    echo "  Expected pattern: vgmstream.*CLI.*decoder"
    echo "  Actual output (first 500 chars):"
    echo "${vgm_output:0:500}"
    exit 1
  fi
  echo "  ✓ vgmstream-cli"

  # On Linux, audit each bundled ELF binary's NEEDED entries: every
  # SONAME must either be in the glibc/loader skip list (delegated to
  # the user's libc) or sit next to the binary in resources/bin/.
  # The `-version` smoke tests above can't catch this on the build
  # host because /usr/lib happens to satisfy the deps — but on a user
  # machine with a different ffmpeg ABI the load fails at runtime
  # (issue #68 on Fedora 44 / Arch).
  if [[ "$PLATFORM" == "linux" ]]; then
    if ! command -v readelf >/dev/null 2>&1; then
      echo_error "readelf not found (apt: binutils) - required to audit bundled binaries' shared library deps"
      exit 1
    fi
    for bin in fluidsynth ffmpeg ffprobe vgmstream-cli; do
      audit_bundled_deps "$bin_dir/$bin" "$bin_dir" || exit 1
    done
    # Also audit each bundled .so's own NEEDED entries. ldd-on-the-top-
    # level-binary usually resolves the full transitive closure, but
    # dlopen-resolved deps and interposer libs can slip through that
    # traversal. Auditing every bundled .so closes the gap.
    for so in "$bin_dir"/*.so*; do
      [ -f "$so" ] || continue
      audit_bundled_deps "$so" "$bin_dir" || exit 1
    done
    echo "  ✓ shared-library audit"
  fi

  echo_summary "All bundled binaries verified"
  echo ""
}

# Asserts every NEEDED SONAME in $1 is either in the glibc skip list
# or present as a file in $2 (the bundle directory). Returns non-zero
# with a specific error if any SONAME is unsatisfied.
audit_bundled_deps() {
  local bin_path="$1"
  local bundle_dir="$2"
  local missing=()
  local soname
  while IFS= read -r soname; do
    [ -n "$soname" ] || continue
    is_skipped_lib "$soname" && continue
    [ -f "$bundle_dir/$soname" ] && continue
    missing+=("$soname")
  done < <(readelf -d "$bin_path" 2>/dev/null | awk -F'[][]' '/\(NEEDED\)/ {print $2}')

  if [ ${#missing[@]} -gt 0 ]; then
    echo_error "Bundled $(basename "$bin_path") needs shared libs that are not in resources/bin/:"
    for soname in "${missing[@]}"; do
      echo "    - $soname"
    done
    echo "  Fix: extend scripts/bundle-binaries.sh so these SONAMEs are bundled (or add them to the glibc skip list if they MUST come from the host libc)."
    return 1
  fi
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
    # Call electron-builder directly. The package.json `dist:*` scripts
    # chain `build:native && bundle && build:ts && electron-builder`,
    # but build-common.sh's main() has already run all three of those
    # explicitly. Going through `npm run dist:*` would re-run them, which
    # on macOS is actively destructive: build:native rebuilds RsCli into
    # resources/bin/rscli/ and overwrites the codesigned versions
    # produced by sign-macos-binaries.sh, so notarization sees unsigned
    # nested binaries and rejects the bundle.
    #
    # `--publish never` is required: on a tag push electron-builder
    # defaults to auto-publishing to GitHub Releases and then errors out
    # without GH_TOKEN. The workflow has a dedicated `release` job that
    # publishes via softprops/action-gh-release after artifact upload —
    # the build job just needs to produce artifacts, not publish them.
    local builder_platform
    case "$PLATFORM" in
        linux)   builder_platform="--linux" ;;
        macos)   builder_platform="--mac" ;;
        windows) builder_platform="--win" ;;
        *)
            echo_error "Unsupported packaging platform: $PLATFORM"
            exit 1
            ;;
    esac
    npx electron-builder "$builder_platform" --publish never
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
