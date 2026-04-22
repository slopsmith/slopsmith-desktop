# DevContainer build environment

Everything needed to build Slopsmith Desktop in a reproducible,
CI-identical environment using Docker.

## Why

Slopsmith Desktop compiles a native C++ audio engine (JUCE), bundles a
portable Python runtime, and packages via electron-builder — all of
which can produce subtly different artifacts on different distros
(glibc, system library versions, Node/Python provenance). This
container pins everything to what the GitHub Actions `ubuntu-22.04`
runner uses so local AppImage builds match CI.

## Contents

| File | Purpose |
|---|---|
| `Dockerfile` | Ubuntu 22.04 base image; versions read from `../.build-config.json` |
| `devcontainer.json` | VS Code Dev Containers configuration |
| `README.md` | This file |

The Docker-wrapped one-shot build lives at
[`../scripts/build-linux-release.sh`](../scripts/build-linux-release.sh).

Versions of Node, Python, .NET, Electron, CMake, and the Ubuntu base
are defined in [`../.build-config.json`](../.build-config.json). CI and
this container both read from it, so no version drift.

## Prerequisites

- [Docker](https://docs.docker.com/get-docker/) installed and running
- The [Slopsmith](https://github.com/byrongamatos/slopsmith) server
  repository cloned **adjacent** to this one:
  ```
  your-projects/
  ├── slopsmith/
  └── slopsmith-desktop/
  ```

## Quick start

### VS Code (recommended)

With the [Dev Containers extension](https://marketplace.visualstudio.com/items?itemName=ms-vscode-remote.remote-containers):

1. Open the project in VS Code
2. Run **Dev Containers: Reopen in Container**
3. Wait for first-time image build + `npm install` (via `postCreateCommand`)
4. Build the release: `npm run dist:linux`
5. Artifacts in `release/` are visible on the host

### Docker only

```bash
./scripts/build-linux-release.sh
```

Builds the image, runs the build inside a container, and emits
`.AppImage` + `.deb` into `release/` on the host.

## Build output

| File | Description |
|---|---|
| `release/*.AppImage` | Portable Linux executable (self-contained) |
| `release/*.deb` | Debian/Ubuntu package |

Both appear on your host filesystem — test them immediately without
touching the container.

## How it works

### The image

The Dockerfile layers onto `mcr.microsoft.com/devcontainers/base:ubuntu-22.04`:
- **Node.js** from NodeSource, version per `.build-config.json`
- **Python 3.12** from the deadsnakes PPA (Ubuntu 22.04's main repos
  only ship 3.10)
- **.NET** via the upstream `dot.net` install script
- **System libraries** from `.packages/apt.txt`

### Mounts

Two bind mounts:

1. `slopsmith-desktop` → `/workspace` — source code, node_modules, build
   output. Edits are reflected immediately on the host.
2. `../slopsmith` → `/workspaces/slopsmith` — the Slopsmith server repo
   that `bundle-slopsmith.sh` copies from at bundle time.

### Container lifecycle

The one-shot build container is **not** auto-removed (`--rm` is
deliberately omitted from `build-linux-release.sh`). This lets you
attach a shell after a failed build to diagnose:

```bash
docker exec -it <container-name> /bin/bash
```

Clean up when done:

```bash
docker stop <container-name> && docker rm <container-name>
```

The script prints the container name on completion or failure.

## Troubleshooting

### "Slopsmith repository not found"

Clone the server repo adjacent:

```bash
cd ..
git clone https://github.com/byrongamatos/slopsmith.git
cd slopsmith-desktop
```

### Permission errors on `release/`

UID/GID mismatch between your host user and the container's `vscode`
user (UID 1000). If needed:

```bash
sudo chown -R "$(id -u):$(id -g)" release/
```

### First build is slow

First build downloads Docker layers, installs apt packages, and
bootstraps node_modules. Subsequent builds use layer caching.

### Out of disk space

Electron builds are large (expect ~10 GB free). Clean old images:

```bash
docker system prune -a
```

## CI vs local container

Intentional differences:
- No publishing secrets (`--publish never`)
- Host architecture (GitHub Actions also builds for macOS/Windows)
- No release-draft creation

Everything else — system deps, Node/Python/.NET versions, the bundle
pipeline — is driven by the same `.build-config.json` + `.packages/`
files the CI workflow reads.

## Modifying the build environment

- **System packages**: edit `.packages/apt.txt` (`brew.txt`/`choco.txt`
  for the other platforms)
- **Tool versions**: edit `.build-config.json`
- **Image-level changes**: edit `Dockerfile`; rebuild with
  `./scripts/build-linux-release.sh` (or VS Code: **Dev Containers:
  Rebuild Container**)

## See also

- [GitHub Actions workflow](../.github/workflows/build.yml) — what this
  container replicates
- [Build architecture](../docs/BUILD_ARCHITECTURE.md) — full script
  layout + conventions
- [`../package.json`](../package.json) — the npm scripts that drive the
  build
