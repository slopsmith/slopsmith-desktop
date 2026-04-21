# DevContainer Build Environment

This directory contains everything needed to build Slopsmith Desktop in a reproducible, CI-identical environment using Docker.

## Why This Exists

Slopsmith Desktop is a complex Electron application with:
- A native C++ audio engine (JUCE-based, compiled with cmake-js)
- Embedded Python runtime with specific package requirements
- Cross-platform build tooling (Electron Builder)

Building this on different Linux distributions (Arch, Fedora, Gentoo, etc.) can produce subtly different binaries due to:
- Different glibc versions
- Different system library versions
- Different Node.js/Python installation methods

**This DevContainer ensures your local builds produce the exact same binaries as GitHub Actions CI**, which runs on `ubuntu-22.04`.

## What's Inside

| File | Purpose |
|------|---------|
| `Dockerfile` | Ubuntu 22.04 base image; reads versions from `../.build-config.json` |
| `devcontainer.json` | VS Code Dev Containers configuration (optional) |
| `README.md` | This file |

The build script itself is located at `../scripts/build-linux-release.sh` (see below).

**Version Management:** Versions for Node.js, Python, .NET, Electron, and build tools are defined in `../.build-config.json`. This file is the single source of truth used by both the DevContainer and GitHub Actions CI, ensuring no version drift between local and CI builds.

## Prerequisites

- [Docker](https://docs.docker.com/get-docker/) installed and running
- The [Slopsmith](https://github.com/byrongamatos/slopsmith) server repository cloned **adjacent** to this one:
  ```
  your-projects/
  ├── slopsmith/           # The server repository
  └── slopsmith-desktop/   # This repository
  ```

## Quick Start

### Option 1: Using VS Code (Recommended)

If you use VS Code with the [Dev Containers extension](https://marketplace.visualstudio.com/items?itemName=ms-vscode-remote.remote-containers):

1. Open the project in VS Code
2. Run command: **Dev Containers: Reopen in Container**
3. Wait for the container to build (first time only) and dependencies to install
4. Build the release:
   ```bash
   npm run dist:linux
   ```
5. Find your binaries in the `release/` directory (visible on your host)

### Option 2: Using Docker Only

```bash
# One-shot build - outputs to ./release/
./scripts/build-linux-release.sh
```

This script will:
- Verify the Slopsmith repository is present
- Build the Docker image
- Run the build process
- Output AppImage and .deb to `./release/`

## Build Output

After a successful build, you'll find:

| File | Description |
|------|-------------|
| `release/*.AppImage` | Portable Linux executable (self-contained) |
| `release/*.deb` | Debian/Ubuntu package |

These files appear in your **host filesystem** (not just inside the container), so you can test them immediately.

## Testing Your Build

The AppImage runs on your host system—no need to run it inside the container:

```bash
# On your host system
./release/Slopsmith-*.AppImage
```

## How It Works

### The Container

The `Dockerfile` creates an environment matching GitHub Actions:
- **Base**: `mcr.microsoft.com/devcontainers/base:ubuntu-22.04`
- **Node.js**: Version 22 (via NodeSource)
- **Python**: Version 3.12 (via deadsnakes PPA)
- **.NET**: Version 10.0 (for building RsCli)
- **System libraries**: All audio/graphics libraries from the CI workflow

### Mounts

When the container runs, two directories are mounted:

1. **This repository** (`slopsmith-desktop`) → `/workspace` in container
   - Source code, node_modules, build output all live here
   - Changes are immediately reflected on your host

2. **Slopsmith repository** (`slopsmith`) → `/workspaces/slopsmith` in container
   - Required by `scripts/bundle.sh` during the build
   - Must be at `../slopsmith` relative to this repo

### One-Shot Philosophy

Unlike typical DevContainers used for ongoing development, this container is **one-shot**:
- It builds the image
- Runs the build process
- Outputs your binaries
- Removes itself (`--rm`)

This keeps your Docker environment clean and ensures every build starts fresh.

## Troubleshooting

### "Slopsmith repository not found"

Make sure you've cloned the Slopsmith repository adjacent to this one:

```bash
# If you're in slopsmith-desktop/
cd ..
git clone https://github.com/byrongamatos/slopsmith.git
cd slopsmith-desktop
```

### Permission Denied Errors

If you see permission errors with the `release/` directory, it's likely a UID/GID mismatch between your host user and the container's `vscode` user (UID 1000). The files should still be readable, but you might need to adjust ownership:

```bash
sudo chown -R $(id -u):$(id -g) release/
```

### Build Takes Forever

First build downloads and installs many dependencies. Subsequent builds use Docker layer caching and are much faster.

### Out of Disk Space

Electron builds are large. Ensure you have at least 10GB free. Clean up old Docker images if needed:

```bash
docker system prune -a
```

## Differences from CI

This container matches the GitHub Actions `ubuntu-22.04` runner as closely as possible. The only intentional differences:

1. **No secrets**: GitHub tokens for publishing are not available locally
2. **Publishing disabled**: Builds are `--publish never` by default
3. **Architecture**: Matches your host (GitHub Actions builds for x64, arm64, etc.)

## For Developers: Understanding the Build

If you want to understand what happens during `npm run dist:linux`:

1. **Bundle resources** (`npm run bundle`): 
   - Copies Slopsmith server from `../slopsmith`
   - Creates portable Python environment
   - Copies system binaries (ffmpeg, etc.)

2. **Compile TypeScript** (`npm run build:ts`):
   - Compiles `src/main/**/*.ts` to `dist/`

3. **Package** (`electron-builder --linux`):
   - Creates AppImage (portable)
   - Creates .deb (package)
   - Both output to `release/`

The native C++ addon (`slopsmith_audio.node`) is built during `npm install` by `cmake-js`.

## Customization

If you need to modify the build environment:

- **Update system packages**: Edit `Dockerfile` apt-get section
- **Change Node/Python versions**: Edit `Dockerfile` installation steps
- **Add VS Code extensions**: Edit `devcontainer.json` -> `customizations.vscode.extensions`
- **Mount additional directories**: Edit `devcontainer.json` or `build.sh`

Remember: Changes to the `Dockerfile` require rebuilding the image.

## See Also

- [GitHub Actions Workflow](../.github/workflows/build.yml) - The CI configuration this container replicates
- [Electron Builder Configuration](../package.json) - Build targets and settings
- [Bundle Script](../scripts/bundle.sh) - What gets packaged into the app
