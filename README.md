# Slopsmith Desktop

Standalone cross-platform desktop app that wraps [Slopsmith](https://github.com/byrongamatos/slopsmith) with integrated VST hosting, amp modeling, audio I/O, and full plugin support.

## Features

- **Slopsmith Web UI** in an embedded webview — full library browser, 3D highway renderer, all plugins
- **VST3/AU Plugin Host** — load any guitar amp sim (Guitar Rig, AmpliTube, Neural DSP, ToneX, etc.)
- **Low-latency Audio I/O** — ASIO (Windows), CoreAudio (Mac), JACK/ALSA (Linux)
- **Built-in NAM** (Neural Amp Modeler) — free amp modeling with community .nam models
- **Cabinet IR Loader** — convolution-based cab simulation
- **Pitch Detection** — native YIN algorithm feeding into note detection
- **Signal Chain Builder** — drag and drop VST/NAM/IR processors
- **Plugin Manager** — install/update/remove Slopsmith plugins via git
- **Cross-platform** — Windows 10+, macOS 12+, Linux

## Install

Prebuilt installers for the latest tagged release are published on the
[GitHub Releases page](https://github.com/byrongamatos/slopsmith-desktop/releases/latest).

| Platform | Download | Notes |
|----------|----------|-------|
| Windows 10/11 (x64) | `Slopsmith.Setup.<version>.exe` | NSIS installer. On first run Windows SmartScreen may warn — click *More info → Run anyway*. |
| macOS 12+ (Apple Silicon) | `Slopsmith-<version>-arm64.dmg` | Signed & notarized. Intel Macs are not currently published — build from source. |
| Linux (x86_64) | `Slopsmith-<version>.AppImage` | `chmod +x` then run. Portable, no install step. |
| Debian / Ubuntu (x86_64) | `slopsmith-desktop_<version>_amd64.deb` | `sudo apt install ./slopsmith-desktop_<version>_amd64.deb` |

> **First launch may take a minute or two** while ML model caches populate
> in the app cache directory. Subsequent launches are fast.

There is currently no Homebrew, winget, Chocolatey, Scoop, Flatpak, or
Snap distribution — download directly from Releases. The app does not
yet ship an auto-updater; check Releases periodically for new versions.

## Architecture

```
┌──────────────────────────────────────────────────┐
│  Electron Shell                                    │
│                                                    │
│  ┌──────────────┐    ┌─────────────────────────┐  │
│  │ Audio Engine  │    │ Webview                 │  │
│  │ (JUCE C++)   │    │ (Slopsmith UI)          │  │
│  │              │    │                         │  │
│  │ Guitar In    │    │  Highway renderer       │  │
│  │   │          │    │  Library browser        │  │
│  │ VST Host ◄──►│◄──►│  Note detection        │  │
│  │   │          │    │  All plugins            │  │
│  │ Output       │    │                         │  │
│  │              │    │                         │  │
│  │ Pitch ───────│───►│  Accuracy feedback      │  │
│  │ Detection    │    │                         │  │
│  └──────────────┘    └─────────────────────────┘  │
│         │                       │                  │
│         │ N-API            localhost:8000           │
│         ▼                       ▼                  │
│  ┌──────────────┐    ┌─────────────────────────┐  │
│  │ Node.js      │    │ Python Subprocess       │  │
│  │ (IPC Bridge) │    │ (Slopsmith server.py)   │  │
│  └──────────────┘    └─────────────────────────┘  │
└──────────────────────────────────────────────────┘
```

## Development Setup

### Prerequisites

- Node.js 22+
- Python 3.12+
- CMake 3.22+
- Git
- [Slopsmith](https://github.com/byrongamatos/slopsmith) — resolved in this order:
  1. `$SLOPSMITH_DIR` env var
  2. `../slopsmith/` (sibling clone, recommended)
  3. `~/Repositories/slopsmith/` (legacy)

  An explicit `$SLOPSMITH_DIR` is used verbatim — if it is wrong, startup
  fails with a clear error rather than falling back. On Windows, set it to a
  native path (`C:\src\slopsmith`), not an MSYS/Git-Bash path
  (`/c/src/slopsmith`), which Node resolves against the current drive root.

**Linux:**
```bash
# Ubuntu/Debian
sudo apt install libasound2-dev libjack-jackd2-dev libfreetype-dev \
  libx11-dev libxrandr-dev libxcursor-dev libxinerama-dev pkg-config cmake \
  ffmpeg
# vgmstream-cli is not in the apt repos — install a prebuilt binary from
# https://github.com/vgmstream/vgmstream/releases (download the Linux CLI
# artifact and place vgmstream-cli on your PATH).

# Arch/Manjaro
sudo pacman -S alsa-lib jack2 freetype2 libx11 libxrandr libxcursor libxinerama pkgconf cmake ffmpeg
yay -S vgmstream-cli-bin
```

**macOS:**
```bash
xcode-select --install
brew install cmake pkg-config ffmpeg vgmstream
```

> **Note:** Homebrew's `ffmpeg` 8.1.1+ no longer enables the `libvorbis`
> encoder. Dev mode still runs — Sloppak conversion falls back to ffmpeg's
> lower-quality built-in vorbis encoder — but packaged macOS builds bundle a
> static ffmpeg with `--enable-libvorbis` for full-quality `.ogg` output.
> `setup-dev.sh` prints a `[WARN]` when the encoder is absent.

### Build

```bash
# Clone with submodules
git clone --recursive https://github.com/byrongamatos/slopsmith-desktop
cd slopsmith-desktop

# Setup
./scripts/setup-dev.sh

# Build audio engine (JUCE native addon)
npm run build:audio

# Build TypeScript
npm run build:ts

# Run
npm run dev
```

### Build for Distribution

```bash
npm run dist:linux   # .AppImage + .deb
npm run dist:mac     # .dmg
npm run dist:win     # .exe installer
```

### Reproducible Linux builds

To match the GitHub Actions `ubuntu-22.04` build environment exactly
(same Node.js, Python, .NET, and system library versions), build
inside the DevContainer:

**Prerequisites**
- [Docker](https://docs.docker.com/get-docker/)
- The [Slopsmith](https://github.com/byrongamatos/slopsmith) server repository
  cloned as a sibling at `../slopsmith`. The DevContainer bind-mounts that exact
  path (`.devcontainer/devcontainer.json`) and its `postCreateCommand` fails
  fast if it is missing — unlike the host build, it does **not** honour
  `$SLOPSMITH_DIR` or `~/Repositories/slopsmith`.

**VS Code**
```bash
# With the Dev Containers extension:
# Dev Containers: Reopen in Container
npm run dist:linux
```

**Docker only**
```bash
./scripts/build-linux-release.sh
```

Both paths write AppImage + .deb to `./release/` on the host. See
[`docs/BUILD_ARCHITECTURE.md`](docs/BUILD_ARCHITECTURE.md) for the
full script layout and
[`.devcontainer/README.md`](.devcontainer/README.md) for container
specifics.

## Audio Engine

The audio engine is a JUCE C++ library compiled as a Node.js native addon via cmake-js. It runs headless (no GUI) and communicates with Electron via N-API.

### Signal Chain

```
Guitar Input → [Input Gain] → [VST/NAM/IR Chain] → [Output Gain] → Speakers
                    │
                    └→ [Pitch Detector] → Webview (note detection)
```

### Supported Formats

| Platform | VST3 | AU | LV2 | ASIO | CoreAudio | JACK | ALSA |
|----------|------|----|-----|------|-----------|------|------|
| Windows  | Yes  | -  | -   | Yes  | -         | -    | -    |
| macOS    | Yes  | Yes| -   | -    | Yes       | -    | -    |
| Linux    | Yes  | -  | Yes | -    | -         | Yes  | Yes  |

### NAM (Neural Amp Modeler)

Free amp modeling using community-created neural network models. Download .nam files from:
- [ToneHunt](https://tonehunt.org)
- [NAM Model Database](https://github.com/sdatkinson/NeuralAmpModelerPlugin/wiki/Models)

## Slopsmith Plugins

All Slopsmith plugins work in the desktop app. The embedded Python server runs the same `server.py` and discovers plugins the same way.

### Installing Plugins

Use the Plugin Manager (Plugins tab) or manually. The plugins directory varies by platform:

| Platform | Plugins directory |
|----------|-------------------|
| macOS    | `~/Library/Application Support/slopsmith-desktop/plugins/` |
| Linux    | `~/.config/slopsmith-desktop/plugins/` |
| Windows  | `%APPDATA%\slopsmith-desktop\plugins\` |

Clone directly into the plugins directory, or symlink a repo from elsewhere (symlinks are followed).

**macOS:**
```bash
mkdir -p ~/Library/Application\ Support/slopsmith-desktop/plugins
git clone https://github.com/user/slopsmith-plugin-foo \
  ~/Library/Application\ Support/slopsmith-desktop/plugins/slopsmith-plugin-foo
# or symlink — always use an absolute path to avoid a broken self-referencing symlink
ln -s /absolute/path/to/slopsmith-plugin-foo \
  ~/Library/Application\ Support/slopsmith-desktop/plugins/slopsmith-plugin-foo
```

**Linux:**
```bash
mkdir -p ~/.config/slopsmith-desktop/plugins
git clone https://github.com/user/slopsmith-plugin-foo \
  ~/.config/slopsmith-desktop/plugins/slopsmith-plugin-foo
```

Restart the app after adding plugins.

## Reporting issues

File bugs and feature requests on the
[issue tracker](https://github.com/byrongamatos/slopsmith-desktop/issues).
Please include your OS / version, the Slopsmith Desktop version (visible
in the title bar or in *About*), and steps to reproduce.

## License

Slopsmith Desktop is licensed under the GNU Affero General Public
License v3.0 — see [`LICENSE`](LICENSE) for the full text.

The audio engine statically links [JUCE 8](https://juce.com/), which is
[AGPL-3.0 when not used under JUCE's commercial licence](https://github.com/juce-framework/JUCE/blob/master/LICENSE.md);
the combined desktop binary inherits AGPL-3.0 via that linkage.

Bundled and build-time third-party components — JUCE, NAM, RTNeural,
Rocksmith2014.NET / `rscli`, FluidSynth (Windows), the GeneralUser GS
SoundFont, the embedded CPython runtime, Electron, and runtime npm
dependencies — and their respective licences are listed in
[`THIRD_PARTY_LICENSES.md`](THIRD_PARTY_LICENSES.md).

The Slopsmith server bundled inside this app
([byrongamatos/slopsmith](https://github.com/byrongamatos/slopsmith))
is also distributed under AGPL-3.0-only.
