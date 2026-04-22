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
- [Slopsmith](https://github.com/byrongamatos/slopsmith) cloned at `../slopsmith/` (preferred) or `~/Repositories/slopsmith/`

**Linux:**
```bash
# Ubuntu/Debian
sudo apt install libasound2-dev libjack-jackd2-dev libfreetype-dev \
  libx11-dev libxrandr-dev libxcursor-dev libxinerama-dev pkg-config cmake

# Arch/Manjaro
sudo pacman -S alsa-lib jack2 freetype2 libx11 libxrandr libxcursor libxinerama cmake
```

**macOS:**
```bash
xcode-select --install
brew install cmake pkg-config
```

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
- The [Slopsmith](https://github.com/byrongamatos/slopsmith) server
  repository cloned at `../slopsmith/`

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

Use the Plugin Manager (Plugins tab) or manually:
```bash
cd ~/.config/slopsmith-desktop/plugins/
git clone https://github.com/user/slopsmith-plugin-foo
# Restart the app
```

## License

MIT
