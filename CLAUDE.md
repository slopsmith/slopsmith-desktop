# Slopsmith Desktop — AI Agent Guide

Standalone cross-platform Electron app that wraps the **[`byrongamatos/slopsmith`](https://github.com/byrongamatos/slopsmith)** web app with a native JUCE audio engine, VST3/AU/LV2 plugin hosting, NAM (Neural Amp Modeler), cabinet IR loading, and low-latency device I/O.

The slopsmith repo carries the library / chart-parsing / WebSocket highway / plugin-system / visualization side. **This repo carries the audio engine + the Electron shell.** Most code paths a contributor touches either live in `src/audio/` (C++) or `src/main/` (TypeScript Electron main process); the renderer just embeds the slopsmith web UI.

## Repo at a glance

```
src/
  audio/                 C++ JUCE engine + ML detection + plugin hosting (compiled to slopsmith_audio.node)
    AudioEngine.cpp/.h   Device I/O, audio callback, input ring buffer, level meters
    PitchDetector.*      YIN-based monophonic pitch detection
    MlNoteDetector.*     Spotify Basic Pitch via ONNX Runtime (PIMPL'd; SLOPSMITH_ONNX_SUPPORT-gated)
    ChordScorer.*        Constraint-based polyphonic chord scorer (FFT + harmonic comb)
    NoteVerifier.*       Background-thread continuous chart verification (modern scoring path)
    OnsetDetector.*      Spectral-flux pick-attack detection (feeds NoteVerifier)
    SignalChain.*        Signal-chain orchestrator (NAM / IR / VST3 / noise gate / tone polish)
    Sandbox/             Subprocess-based VST3 hosting (sandbox-by-default on Windows)
    NodeAddon.cpp        N-API binding — surfaces engine methods to JS
  main/                  Electron main process (TypeScript)
    audio-bridge.ts      IPC handlers: ipcMain.handle('audio:…', …) wraps every NodeAddon method
    preload.ts           contextBridge.exposeInMainWorld('slopsmithDesktop', { audio: { … } })
    python.ts            Spawns the Python sibling server (port 18000)
    main.ts              BrowserWindow setup, app lifecycle
  renderer/              Renderer-side desktop-only plugin scripts
    screen.js/.html      The audio_engine plugin (device picker, gains, NAM/IR/VST chain UI)
    plugin-manager/      The plugin_manager plugin (install/update/remove slopsmith plugins via git)
docs/                    Developer documentation (see below)
scripts/                 Build + bundling shell scripts (bundle-binaries, bundle-slopsmith, etc.)
JUCE/                    JUCE framework submodule
```

## Where to look for things

### Documentation (`docs/`)

- [`docs/audio-engine-architecture.md`](docs/audio-engine-architecture.md) — **start here for the audio engine.** Component map, threading model, two-tier detection (YIN + ML), NoteVerifier, IPC surface, build flags, threading invariants.
- [`docs/BUILD_ARCHITECTURE.md`](docs/BUILD_ARCHITECTURE.md) — npm scripts, shell-script modularity, `.build-config.json` pins, platform bundlers.
- [`docs/SANDBOX-DESIGN.md`](docs/SANDBOX-DESIGN.md) — VST3 sandbox-by-default rationale + implementation.
- [`docs/VST-SANDBOX-DIAG.md`](docs/VST-SANDBOX-DIAG.md) — diagnosing sandbox subprocess crashes.

### Cross-repo

The renderer-side picture — how `note_detect` consumes the IPC methods this repo exposes, registers `setNoteStateProvider`, and feeds the highway — lives in **`byrongamatos/slopsmith`**:

- [`slopsmith/docs/realtime-scoring-pipeline.md`](https://github.com/byrongamatos/slopsmith/blob/main/docs/realtime-scoring-pipeline.md) — end-to-end trace, audio sample → lit gem.
- [`slopsmith/docs/note-state-provider.md`](https://github.com/byrongamatos/slopsmith/blob/main/docs/note-state-provider.md) — the `setNoteStateProvider` / `bundle.getNoteState` contract.
- [`slopsmith/docs/visualization-feedback-guide.md`](https://github.com/byrongamatos/slopsmith/blob/main/docs/visualization-feedback-guide.md) — practical guide for plugin authors.
- [`slopsmith/CLAUDE.md`](https://github.com/byrongamatos/slopsmith/blob/main/CLAUDE.md) — slopsmith's own AI agent guide (plugin system, WebSocket protocol, visualization contracts).

Touching the audio engine in this repo almost always means a renderer-side companion change in `slopsmith`, or vice versa. Trace the request across both repos before editing.

## Build commands (run from repo root)

| Command | Frequency | Notes |
|---|---|---|
| `npm run build:audio` | On C++ / submodule / `src/audio/CMakeLists.txt` changes | JUCE + RTNeural + ONNX + Signalsmith → `build/Release/slopsmith_audio.node`. Cold build 5–15 min; incremental fast. **All `electron.exe` processes must be closed** — Windows holds an exclusive write lock on loaded `.node` files; the link step otherwise fails with `LNK1104: cannot open file 'slopsmith_audio.node'`. |
| `npm run build:ts` | Each dev launch (fast) | TypeScript compile for the main process + preload. Run automatically by `npm run dev`. |
| `npm run dev` | Each session | `build:ts && electron .`. The renderer loads the sibling `slopsmith/` Python server at `http://localhost:18000`. |
| `npm run dist` | Per release | Full bundle with `scripts/bundle-slopsmith.sh`, `scripts/bundle-binaries.sh`, installer generation. |

## Build flags

| Flag | Default | Effect when off |
|---|---|---|
| `SLOPSMITH_ONNX_SUPPORT` | on (where ONNX Runtime is available) | `MlNoteDetector` is an inert no-op; engine falls back to YIN-only detection. See `src/audio/MlNoteDetector.h:14-19` and the PIMPL boundary at `MlNoteDetector.cpp:6`. |

## Threading invariants

The audio engine has three thread classes and the contract between them is load-bearing — see [`docs/audio-engine-architecture.md`](docs/audio-engine-architecture.md) § "Threading invariants" for the full version. Short version:

1. **Audio device thread** (`AudioEngine::audioDeviceIOCallbackWithContext`): no allocations, no blocking locks. Writes to `inputFrameRing` via release-store on the write index; reads from atomics.
2. **Worker threads** (`PitchDetector::run`, `MlNoteDetector::run`, `NoteVerifier::run`): own their state, publish snapshots under a `juce::CriticalSection` or via atomics, read from `inputFrameRing` via `getInputFrame()` / `getInputSince()` (acquire-load).
3. **N-API main thread**: polls snapshots and marshals to JS objects. Never holds a lock the audio thread can also try to take.

Breaking these — a `juce::Logger::writeToLog(...)` in the callback that allocates a string, a `std::vector::push_back` in a worker that triggers a reallocation while audio is reading — is the most common source of subtle real-time bugs. Run with the JUCE RT-safety checks enabled when working in the callback.

## Common gotchas

1. **Audio rebuild lock**: see the table above. Close Electron before `npm run build:audio` on Windows.
2. **Renderer plugins are desktop-only**: `src/renderer/screen.js` (audio_engine) and `src/renderer/plugin-manager/screen.js` are bundled into the slopsmith server plugins dir by `scripts/bundle-slopsmith.sh` during `npm run dist`. In dev mode (`npm run dev`) they're copied manually into the sibling `slopsmith/plugins/` directory — see the workspace-level CLAUDE.md if you're working in a dev workspace.
3. **The slopsmith server runs on port 18000, not 8000**: `src/main/python.ts` spawns it as a subprocess on the non-default port to avoid clashing with a separately-running slopsmith Docker container.
4. **`ELECTRON_RUN_AS_NODE` must be unset**: if your shell exports this env var, Electron launches as plain Node and `require('electron')` returns the binary path string instead of `{app, BrowserWindow, …}`. Symptom: `TypeError: Cannot read properties of undefined (reading 'start')` on `crashReporter.start` at launch.
5. **VST3 plugins run sandboxed by default** ([PR #247](https://github.com/byrongamatos/slopsmith-desktop/pull/247)): a crashing plugin no longer takes down the audio engine. See [`docs/SANDBOX-DESIGN.md`](docs/SANDBOX-DESIGN.md) for the model and [`docs/VST-SANDBOX-DIAG.md`](docs/VST-SANDBOX-DIAG.md) for diagnostic steps when a plugin fails to load.

## Versioning

- `VERSION` (repo root) is the single source of truth. Format: plain semver (e.g. `0.2.9-alpha.6`).
- Releases tag the desktop build; that release job fires a `repository_dispatch` event into `byrongamatos/slopsmith` to keep its VERSION in sync.
- `CHANGELOG.md` follows [Keep a Changelog](https://keepachangelog.com/) format. Update the `[Unreleased]` section with each PR.

## Git workflow

- **Never push directly to main** — always feature branch + PR.
- **`origin` points to the canonical repo** (`byrongamatos/slopsmith-desktop`). Contributors with write access push feature branches to `origin`; others fork and PR from there.
- **DCO sign-off required** — commit with `git commit -s` so the trailer `Signed-off-by: …` is added. PR checks reject unsigned commits.
- **Plugin gitlinks**: this repo embeds JUCE + RTNeural + NAM + Signalsmith as submodules. After a `git pull` that touches `.gitmodules` or a submodule SHA, run `git submodule update --init --recursive` before rebuilding.

## Off-branch worktrees

The slopsmith dev workflow encourages feature branches kept checked out alongside main work. For chores / docs / hotfixes that should branch from `main` rather than disturb a feature checkout, use a sibling worktree:

```bash
git fetch origin main --quiet
git worktree add -b chore/<name> ../slopsmith-desktop-<name> origin/main
# … work, commit -s, push, open PR …
# after merge:
git worktree remove ../slopsmith-desktop-<name>
git branch -d chore/<name>
```
