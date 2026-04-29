# Build Scripts

Unified build system for Slopsmith Desktop supporting Linux (via Docker), macOS, and Windows.

## Quick Start

```bash
./scripts/build-release.sh
```

## Call Hierarchy

```
┌─────────────────┐
│ GitHub Actions  │
│ build.yml       │
└────────┬────────┘
         │
         │  (calls the same script everywhere)
         │
         ▼
┌──────────────────┐
│ build-release.sh │  Platform dispatcher
└────────┬─────────┘
         │
         │  Detects host OS:
         │  - Linux → build-linux-docker.sh
         │  - macOS → build-macos.sh
         │  - Windows → build-windows.sh
         │
         ▼
┌──────────────────────────────────────┐
│ For Linux: Docker wrapper            │
│ build-linux-docker.sh →              │
│     Docker container:                │
│     ./build-linux-ubuntu.sh          │
└──────────────────────────────────────┘
         │
         │  Sources:
         │
         ▼
┌─────────────────┐
│ build-common.sh │  Shared build logic (~250 lines)
└─────────────────┘
     ▲     ▲     ▲
     │     │     │
     │     │     ├─── Platform-specific implementations:
     │     │           install_system_deps()
     │     │           bundle_python_impl()
     │     │           bundle_binaries_impl()
     │     │
     │     └─ build-macos.sh
     │        build-windows.sh
     │        build-linux-ubuntu.sh
     │
     └─ platform: mac / win / linux
```

## How It Works

1. **build-release.sh** - Platform dispatcher. Detects OS and routes to the right build script.

2. **Linux builds** - Always Docker-based for reproducibility:
   - `build-linux-docker.sh` → Docker container → `build-linux-ubuntu.sh` → packages

3. **Native builds** - macOS and Windows run directly on host:
   - `build-macos.sh` - Uses Homebrew dependencies
   - `build-windows.sh` - Uses Git Bash, downloads binaries

4. **build-common.sh** - Shared logic sourced by platform scripts:
   - Validates environment (Node.js, Python, .NET)
   - Runs npm install, builds C++ engine, bundles resources
   - Calls platform-specific functions for: dependency installation, Python bundling, binary bundling

## Platform-Specific Scripts

### Files

| Script | Purpose | Requirements | Output |
|--------|---------|--------------|--------|
| `build-linux-docker.sh` | Reproducible Docker build | Docker, adjacent slopsmith repo | `.AppImage`, `.deb` |
| `build-linux-ubuntu.sh` | Native Ubuntu build | Ubuntu/Debian + apt | `.AppImage`, `.deb` |
| `build-macos.sh` | Native macOS build | Homebrew, Xcode CLI | `.dmg`, `.zip` |
| `build-windows.sh` | Native Windows build | Git Bash, Node.js, Python, .NET | `.exe` installer |

### Two-Layer Ubuntu Builds

Most Linux distributions don't have identical package versions. Using Docker ensures the build is reproducible:

- **Direct use**: `./scripts/build-linux-docker.sh`
- **Inside container**: Runs `./scripts/build-linux-ubuntu.sh`
- **Why**: Guarantees identical builds across different Linux distros

### Platform-Specific Functions

Each platform script implements 3 functions that `build-common.sh` calls:

```bash
install_system_deps() {
  # Platform-specific: apt install, brew install, choco install, or downloads
}

bundle_python_impl() {
  # Linux: copy system Python
  # macOS: copy system Python
  # Windows: download embeddable Python zip
}

bundle_binaries_impl() {
  # Linux: copy existing + patchelf
  # macOS: copy existing + dylibbundler
  # Windows: download binaries (ffmpeg, vgmstream, fluidsynth)
}
```

## Requirements

| Platform | Requirements |
|----------|--------------|
| **Linux (Docker)** | Docker, adjacent slopsmith repo |
| **Linux (native)** | Ubuntu/Debian, sudo, Node.js 22+, Python 3.12+, .NET 6.0+, apt dependencies |
| **macOS** | macOS 11+, Homebrew, Xcode CLI, Node.js 22+, Python 3.12+, .NET 6.0+ |
| **Windows** | Windows 10/11, Git for Windows + Bash, Node.js 22+, Python 3.12+, .NET 6.0+ |

**Windows Note:** These scripts must run in Git Bash (MSYS), not cmd.exe or PowerShell. `/tmp` paths are translated by MSYS (e.g., to `C:\msys64\tmp`), which will not work. So for local development outside GitHub Actions, Git Bash is required.

## GitHub Actions

The CI workflow is extremely simple - just calls the same script:

```yaml
# .github/workflows/build.yml
steps:
  # Install platform-specific dependencies (apt, brew, or choco)
  - name: Install dependencies
    run: ...

  # Build using the same script developers use locally
  - name: Build
    shell: bash
    run: ./scripts/build-release.sh
```

Result:
- Local builds and CI use identical code paths
- Build failures can be reproduced and debugged locally
- Workflow is "dumb" - all logic lives in versioned scripts

