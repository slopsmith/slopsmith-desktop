# Audio engine architecture

How the native side of slopsmith-desktop produces the verdicts that the `note_detect` plugin drains and the highway renders. This doc is the engine-side companion to [`realtime-scoring-pipeline.md`](https://github.com/byrongamatos/slopsmith/blob/main/docs/realtime-scoring-pipeline.md) in the `byrongamatos/slopsmith` repo — read that first if you want the cross-process picture; this one focuses on what happens between an audio sample arriving at the device and a verdict landing on the IPC bus.

**Audience**: engineers extending or debugging the C++ engine, the ML detector, or the IPC layer.

## Component map

```
┌─────────────────────────────────────────────────────────────────────┐
│                       Audio device driver thread                    │
│  audioDeviceIOCallbackWithContext   (AudioEngine.cpp:1447)          │
│      ├── per-channel routing → mono mix                             │
│      ├── post-gain write into inputFrameRing (SPSC, 8192 samples)   │
│      ├── pushSamples → PitchDetector (YIN, ~8 kHz analysis)         │
│      ├── pushSamples → MlNoteDetector (Basic Pitch / ONNX, 22050)   │
│      └── SignalChain.processBlock (NAM, IRs, sandboxed VST3, etc.)  │
└────────────────────────┬────────────────────────────────────────────┘
                         │
                         ▼ (lock-free reads, audio thread NEVER blocked)
┌─────────────────────────────────────────────────────────────────────┐
│        Background workers (juce::Thread, low priority)              │
│                                                                     │
│  PitchDetector::run()   — decimate → YIN window → atomic publish    │
│  MlNoteDetector::run()  — resample → ONNX inference → snapshot      │
│  NoteVerifier::run()    — onset + harmonic comb → verdict queue     │
└────────────────────────┬────────────────────────────────────────────┘
                         │
                         ▼ (CriticalSection-guarded snapshots, polled)
┌─────────────────────────────────────────────────────────────────────┐
│                 N-API main thread (NodeAddon.cpp)                   │
│  getPitchDetection / scoreChord / setChart / getNoteVerdicts /      │
│  detectNotes / isMlNoteDetection / getLevels / getSampleRate / ...  │
└────────────────────────┬────────────────────────────────────────────┘
                         │
                         ▼ ipcMain.handle (audio-bridge.ts, Electron preload)
                       renderer (note_detect plugin)
```

The shape of the design is: **audio thread captures, worker threads detect, N-API marshals out**. The audio thread never allocates, never blocks on a mutex, and never crosses an IPC boundary. The worker threads do all the heavy lifting and publish snapshots that the N-API thread polls without taking any audio-side lock.

## Audio device callback

Entry point: `AudioEngine::audioDeviceIOCallbackWithContext(...)` at [`src/audio/AudioEngine.cpp:1447`](../src/audio/AudioEngine.cpp).

What happens, in order, per audio block:

1. **Per-channel input routing**. The device may advertise more channels than we want to score against (multi-channel interfaces; Rocksmith real-tone cable on left of a stereo capture; ASIO with 8 inputs). The atomic `selectedInputChannel` chooses one:
   - `selectedCh >= 0`: that channel is the source.
   - `selectedCh < 0` and `numInputChannels > 1`: average channels 0 and 1.
   - Single-input device: pass-through.
2. **Post-gain mono signal** is written to **`inputFrameRing`** ([`AudioEngine.h:196`](../src/audio/AudioEngine.h)) — a fixed `std::array<std::atomic<float>, 8192>` ring with a release-store write index and relaxed per-slot writes. The capacity (8192 samples = ~170 ms at 48 kHz) is a hard ceiling; consumers that ask for more than this clamp.
3. The same mono signal is `pushSamples()`-ed into:
   - `PitchDetector` (YIN) — for the monophonic dominant-pitch readout.
   - `MlNoteDetector` (Basic Pitch via ONNX) — for the polyphonic active-pitch set.
   Both have their own internal lock-free SPSC FIFOs; the audio thread is non-blocking in both calls.
4. **Signal chain** (NAM, IRs, noise gate, sandboxed VST3 plugins, tone polish) runs on the same audio block via `SignalChain.processBlock`.
5. **Output mix** — backing track, monitor signal, processed signal — goes to the output device. In split-mode duplex (PR #232: ASIO input + non-ASIO output), the input callback writes a packed stereo frame into a separate `outputPendingRing` and the output callback drains it; clock drift between the two devices is absorbed by the ring.

The whole thing is wait-free on the happy path. The only lock the callback even tries is a `juce::ScopedTryLock` around the backing-track mixer in duplex mode — and if it fails, the callback skips the backing mix for that block rather than waiting.

## Two-tier pitch detection

slopsmith carries two detectors and prefers the polyphonic ML one when it's ready.

### YIN (`PitchDetector`)

[`src/audio/PitchDetector.h`](../src/audio/PitchDetector.h), [`src/audio/PitchDetector.cpp`](../src/audio/PitchDetector.cpp).

- Decimates the input to ~8 kHz before running YIN (which is O(N²)), bounding cost.
- Background `juce::Thread` drains the FIFO, applies a Butterworth low-pass, runs YIN over a window of >2 periods of the lowest note of interest.
- Results (`frequency`, `confidence`, `midiNote`, `cents`, `noteName`) are published via `std::atomic` members, readable lock-free from any thread.

### Basic Pitch (`MlNoteDetector`)

[`src/audio/MlNoteDetector.h`](../src/audio/MlNoteDetector.h), [`src/audio/MlNoteDetector.cpp`](../src/audio/MlNoteDetector.cpp).

- Wraps Spotify's Basic Pitch model via ONNX Runtime. PIMPL'd so the engine headers don't pull `onnxruntime_cxx_api.h` (`MlNoteDetector.h:18-19`).
- Resamples to **22050 Hz** (`kModelSampleRate`) and runs inference on a rolling window. The worker sleeps 8 ms between iterations when there's no fresh audio.
- Publishes a per-pitch (`kNumPitches = 88` MIDI notes) snapshot of `{activity, onsetTimeMs, onsetSeq, onsetConf}`, guarded by a `juce::CriticalSection` but read lock-free via atomic copies into snap arrays.
- Build-gated by **`SLOPSMITH_ONNX_SUPPORT`** ([`MlNoteDetector.h:14-16`](../src/audio/MlNoteDetector.h), [`MlNoteDetector.cpp:6`](../src/audio/MlNoteDetector.cpp)). When 0, every method is an inert no-op and `isReady()` returns false forever — the engine silently falls back to YIN.

### Choosing between them

`AudioEngine::getActiveDetection()` prefers the ML detector's dominant pitch when available and falls back to YIN otherwise. `getPitchDetection()` IPC returns the same shape either way — the caller doesn't need to know which detector answered.

Loading happens lazily: the renderer calls `audio:loadNoteModel(path)` once at startup with the bundled model path. Subsequent failures don't unload a working model — the renderer can probe `audio:isMlNoteDetection()` to find out which path is live.

## `ChordScorer` — constraint-based polyphonic scoring

[`src/audio/ChordScorer.h`](../src/audio/ChordScorer.h), [`src/audio/ChordScorer.cpp`](../src/audio/ChordScorer.cpp).

Used when the ML detector isn't available (the YIN-only fallback path), or when something explicitly bypasses ML via `req.bypassMl = true`.

Inputs: a chord context (notes, per-string tuning offsets, capo, optional harmonic-verification flag) and a window of input samples (default 4096, snapshotted from `inputFrameRing` via `getInputFrame()` at [`AudioEngine.cpp:1911`](../src/audio/AudioEngine.cpp)).

Pipeline:

1. `juce::dsp::FFT` → magnitude spectrum.
2. For each chart note, extract energy in the expected fundamental band (string's fundamental ± a configured cents window).
3. Optionally apply a harmonic-comb verification: sum energy at f, 2f, 3f, 4f, 5f and compare to the off-harmonic spectral floor. This rejects false positives where unrelated low-frequency energy lights up an open-string band.
4. Returns `{score, hitStrings, totalStrings, isHit, results[]}` where each result is `{string, fret, hit, bandEnergy, centsDiff, centsError}`.

When the ML detector *is* ready and `req.bypassMl=false`, `scoreChord` short-circuits to a simpler path: for each chart note, ask the ML detector `isPitchActive(expectedMidi)` and pack the booleans into the same `Result` shape. No FFT, no spectrum, no cents estimate (discrete pitch detection only). The renderer's contract is unchanged.

## `NoteVerifier` — background continuous scoring

[`src/audio/NoteVerifier.h`](../src/audio/NoteVerifier.h), [`src/audio/NoteVerifier.cpp`](../src/audio/NoteVerifier.cpp).

This is the modern path. `NoteVerifier` exists because the renderer-side `setInterval` loop that used to drive scoring would black out for 1-3 seconds during dense passages — the event loop got starved by other work — and whole runs of notes never got scored. Moving the loop into the engine fixes that.

Design (quoted from the header):

> A background `juce::Thread` walks the pushed chart against the live playhead and scores each note's timing window exactly once, publishing a verdict the renderer just drains.

Concretely:

- **`setChart(ChartUpdate)`** at [`NoteVerifier.cpp:52`](../src/audio/NoteVerifier.cpp). Called by the renderer once per `playSong()` / arrangement switch. Takes the active arrangement's notes + chord constituents with technique flags (hammer-on/pull-off, slide, bend, palm-mute) and the timing/pitch tolerance settings the user has dialled. Stored under `juce::CriticalSection lock`.
- **`setPlayhead(songTime, playing)`**. The worker doesn't read `AudioEngine::getBackingPosition()` because that's frozen for sloppak (HTML5-routed) songs — the renderer owns the unified playhead and pushes it each detect tick. The worker interpolates between pushes using wall-clock; if a push is stale (>1 s old) it freezes.
- **`run()`** at [`NoteVerifier.cpp:152`](../src/audio/NoteVerifier.cpp). The worker tick:
  1. Drain fresh samples from the input ring via `AudioEngine::getInputSince(readCursor, fresh)`.
  2. Feed them to an **`OnsetDetector`** (spectral-flux, [`src/audio/OnsetDetector.h`](../src/audio/OnsetDetector.h)) for precise pick-attack timing — the harmonic-comb's ~85 ms window is too coarse to time an onset, so a dedicated detector handles that. Legato notes (HO/PO) have no attack and are reported on-time.
  3. Snapshot the chart under the lock; assemble a batch of notes whose timing windows are currently open (`playhead ∈ [t - tolerance, t + tolerance + sustainGrace]`).
  4. Score each open-window note with a harmonic comb. If the comb confirms the pitch present *anywhere* in the window, mark `everPresent = true` and record the best SNR + cents error.
  5. As notes' windows close, push **`Verdict { id, detected, detectedSongTime, centsError, snr }`** onto the drain queue.
- **`drainVerdicts()`** ([`AudioEngine.h:236`](../src/audio/AudioEngine.h)). The N-API thread calls this each `audio:getNoteVerdicts` IPC; verdicts are queue-popped, not re-emitted.

The renderer is reduced to: "push the chart once, drain verdicts at 20 Hz, optionally push playhead in the same call." No more per-tick `scoreChord` IPC for the common case.

## N-API surface (`NodeAddon.cpp`)

[`src/audio/NodeAddon.cpp`](../src/audio/NodeAddon.cpp) is the N-API binding. Every method runs on the Electron main thread (not the audio thread, not a worker), and every method goes through the lock-free atomic snapshots the workers publish. The main thread never reaches into `inputFrameRing` directly except via `getInputFrame()` / `getInputSince()`, which are documented for off-audio-thread reads.

The detection-relevant exports (search `NodeAddon.cpp` for the symbol):

| N-API symbol | Engine call | Returns |
|---|---|---|
| `GetPitchDetection` | `AudioEngine::getActiveDetection()` | `{frequency, confidence, midiNote, cents, noteName}` |
| `ScoreChord` | `AudioEngine::scoreChord(req)` | `{score, hitStrings, totalStrings, isHit, results[]}` |
| `SetChart` | `NoteVerifier::setChart(...)` | `null` on success / downlevel |
| `GetNoteVerdicts` | `NoteVerifier::drainVerdicts()` (+ optional `setPlayhead`) | `[{id, detected, detectedSongTime, centsError, snr}]` |
| `DetectNotes` | `MlNoteDetector::getActiveNotes()` | `{notes: [{midi, confidence, onsetMs, onsetSeq}], sampleRate}` |
| `IsMlNoteDetection` | `MlNoteDetector::isReady()` | `boolean` |
| `GetLevels` | `AudioEngine::getLevels()` | `{inputLevel, outputLevel, inputPeak, outputPeak}` |
| `GetSampleRate` | `AudioEngine::getSampleRate()` | `number` (Hz, ≥ 48000) |
| `LoadNoteModel` | `MlNoteDetector::loadModel(file)` | `boolean` ("ML available after this call") |

The IPC wrappers live in [`src/main/audio-bridge.ts`](../src/main/audio-bridge.ts) (search for `ipcMain.handle('audio:`). Each wrapper feature-detects the corresponding native method (`typeof audio.xxx === 'function'`) so a downlevel addon returns `null` instead of throwing.

## Sandbox + plugin hosting (brief)

VST3 (and selected NAM / IR loader) plugins run in a separate `slopsmith-vst-host.exe` subprocess by default ([PR #247](https://github.com/byrongamatos/slopsmith-desktop/pull/247)). The audio engine talks to each plugin through a `SandboxedProcessor` ([`src/audio/Sandbox/SandboxedProcessor.h`](../src/audio/Sandbox/SandboxedProcessor.h)) that wraps `juce::AudioProcessor` and marshals audio blocks via shared memory + control messages over named pipes. If a plugin crashes, the wrapper observes `isAlive()` going false on a background thread, inserts silence, and the audio thread keeps running.

This is orthogonal to detection — sandboxed plugins are downstream of the input tap, so the detectors always see the raw (pre-plugin) signal regardless of what's hosted. macOS and Linux currently use stub factories that load plugins in-process ([`src/audio/Sandbox/SandboxFactory_stub.cpp`](../src/audio/Sandbox/SandboxFactory_stub.cpp)).

## Split-mode duplex / ASIO + non-ASIO

[PR #232](https://github.com/byrongamatos/slopsmith-desktop/pull/232). The engine supports asymmetric device configurations — ASIO input + WASAPI output, or input-only + any output type. Two `AudioDeviceManager`s, two callback threads, and an SPSC ring (`outputPendingRing`) between them with packed stereo frames so the audio callback never sees a half-written frame on wrap. Clock drift between the two devices is absorbed by the ring; under/overflow counters are exposed via `getDeviceMetrics()` for diagnostics.

This also is orthogonal to detection — the input side feeds `inputFrameRing` the same way regardless of which path is active.

## Build flags

| Flag | Default | Effect when off |
|---|---|---|
| `SLOPSMITH_ONNX_SUPPORT` | on (where ONNX is available) | `MlNoteDetector` is inert; YIN-only fallback. |
| (sandbox-by-default behaviour) | on (Windows) | All hosted plugins run in-process; a crashing plugin can take down the audio engine. |

Linkage details are in [`src/audio/CMakeLists.txt`](../src/audio/CMakeLists.txt). The audio target links against `juce::juce_audio_basics`, `juce::juce_audio_devices`, `juce::juce_audio_formats`, `juce::juce_audio_processors` (headless variant), `juce::juce_core`, `juce::juce_dsp`, `juce::juce_events`. NAM (Neural Amp Modeler) and RTNeural come in via their own submodules. Signalsmith Stretch + Linear were added in PR #230 for pitch-preserving JUCE backing-track speed control.

## Threading invariants

The contract everything else depends on:

1. **Audio thread** (`audioDeviceIOCallbackWithContext`): no allocations, no locks except `juce::ScopedTryLock` (with skip-on-fail). Writes to `inputFrameRing` only via release-store on the write index. Calls `pushSamples()` into the detectors' internal lock-free FIFOs.
2. **Worker threads** (`PitchDetector::run`, `MlNoteDetector::run`, `NoteVerifier::run`): each owns its work, has its own state, publishes snapshots under a `juce::CriticalSection` (or via atomics). Reads from `inputFrameRing` via `getInputFrame` / `getInputSince` (acquire-load on the write index).
3. **N-API main thread**: polls snapshots, marshals to JS objects. Never takes the audio-thread side of any lock. Calls into `setChart` and friends may take the worker's CriticalSection, but those operations are fast (vector swap, atomic store).

Breaking any of these is the most common source of subtle bugs — extra logging in the audio callback, a one-line debug `juce::Logger::writeToLog(...)` allocating a string, a forgotten `std::vector::push_back` in the worker that triggers a reallocation while the audio thread is reading. Add real-time-safe checks (e.g., `JUCE_BREAK_IN_DEBUGGER` on allocation) when working in the callback.

## Where to look next

- [`realtime-scoring-pipeline.md`](https://github.com/byrongamatos/slopsmith/blob/main/docs/realtime-scoring-pipeline.md) in the `slopsmith` repo — the cross-process picture, from `audio:getNoteVerdicts` to the lit gem.
- [`note-state-provider.md`](https://github.com/byrongamatos/slopsmith/blob/main/docs/note-state-provider.md) — the renderer contract `note_detect` publishes against.
- `src/audio/AudioEngine.h` and `.cpp` — the JUCE callback, ring buffers, level meters.
- `src/audio/NoteVerifier.h` opening comment — the design rationale in the author's own words; worth reading before extending the verifier.
- `src/audio/MlNoteDetector.h` opening comment — the ONNX wrapper's contract, build-flag interactions, and the PIMPL boundary.

## Build commands (from `slopsmith-desktop/`)

```bash
npm run build:audio       # JUCE + ONNX + N-API → build/Release/slopsmith_audio.node (5-15 min cold)
npm run build:ts          # TypeScript main + preload
npm run dev               # build:ts && electron .
```

Close all `electron.exe` processes before rebuilding `:audio` — Windows holds an exclusive write lock on loaded `.node` files and the link step will fail with `LNK1104`.
