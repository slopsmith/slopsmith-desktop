# Slopsmith plugin-sandbox — IPC + lifecycle design

Date: 2026-05-13
Companion to: `docs/VST-SANDBOX-DIAG.md`
Status: partially implemented (Windows v1, WIP); macOS/Linux pending

## 1. Topology

```text
┌────────────────────────────────────┐         ┌──────────────────────────────┐
│ Slopsmith Desktop (Electron main)  │         │  slopsmith-vst-host.exe      │
│                                    │         │  (one per sandboxed plugin)  │
│  Node main process                 │         │                              │
│  └── slopsmith_audio.node          │         │  WinMain → main thread       │
│      └── SignalChain               │         │  └── JUCE MessageManager     │
│          └── SandboxedProcessor ◀┐ │         │      └── one AudioPlugin     │
│              ▲                   │  │         │          └── editor (HWND)  │
│              │                   │  │         │                              │
│              │ control:          │  │  pipe   │                              │
│              │ JSON over named   ├──┼─────────┤                              │
│              │ pipe (req/resp +  │  │         │                              │
│              │ events)           │  │         │                              │
│              │                   │  │         │                              │
│              │ audio: shared     │  │  shm    │                              │
│              │ memory ring +     ├──┼─────────┤                              │
│              │ Win32 events      │  │         │                              │
└────────────────────────────────────┘         └──────────────────────────────┘
```

One sandbox process per sandboxed plugin (simplest; matches the PoC). Pooling is a v2
optimisation, not v1.

In-process loading remains the default. A plugin only goes through the sandbox if it
matches a denylist (see §5).

## 2. Process spawn + handshake

```text
slopsmith-vst-host.exe
    --plugin-path "<absolute vst3 path>"
    --control-pipe "\\.\pipe\slopsmith-vst-<uuid>"
    --audio-shm    "Local\slopsmith-vst-<uuid>-audio"
    --audio-event-in  "Local\slopsmith-vst-<uuid>-evt-in"
    --audio-event-out "Local\slopsmith-vst-<uuid>-evt-out"
    --sample-rate 48000 --max-block 1024 --channels 2
```

Spawned via `CreateProcess`. The main process creates the pipe + shm + events first,
then spawns; the sandbox connects on startup. Watchdog: if no `ready` event arrives
within `kReadyTimeoutMs` (30 s — long enough for Qt-using plugins like GR6 to
spin up their QML engine on a cold cache), kill and report failure.

`<uuid>` is a v4 UUID generated per spawn so multiple sandboxes coexist cleanly.

## 3. Control channel — named pipe

Transport: `PIPE_TYPE_BYTE | PIPE_READMODE_BYTE`, bidirectional, overlapped I/O,
with an explicit `[u32 length-LE][body]` framing layer the channel applies on
top. (Message-mode was the original plan, but the sandbox's first `ready` frame
wasn't being delivered to the host I/O thread reliably; byte mode + length
prefixes is what shipped — see commit `2cb9ae9`.)

Framing per message: `[u32 length-LE] [json body]`. JSON is small and human-readable
for logging; the audio fast path is *not* on this channel.

Every request from main→sandbox carries a `requestId`. The sandbox echoes it on the
matching reply. Events the sandbox originates (parameter automation, log lines)
carry `requestId: null`.

### Main → sandbox

| `op` | Status | Payload | Reply |
|---|---|---|---|
| `prepare` | v1 | `{ sampleRate, blockSize }` | `{ ok, latencySamples, numInputs, numOutputs }` |
| `setParameter` | v1 | `{ index, value }` | `{ ok }` (omit reply if `fireAndForget: true`) |
| `getState` | v1 | `{}` | `{ stateBase64 }` |
| `setState` | v1 | `{ stateBase64 }` | `{ ok }` |
| `midiEvent` | v1 | `{ frame, bytes: [..] }` | `{ ok }` (fire-and-forget by default) |
| `openEditor` | v1 | `{}` | `{ hwnd: "0x...", w, h }` |
| `closeEditor` | v1 | `{}` | `{ ok }` |
| `shutdown` | v1 | `{}` | `{ ok }` then sandbox exits 0 |
| `setBlockSize` | planned | `{ blockSize }` | `{ ok }` |
| `resizeEditor` | planned | `{ w, h }` | `{ ok }` |
| `listParameters` | planned | `{}` | `{ params: [{index,name,defaultValue,...}] }` |

Status reflects the current dispatcher in `src/vst-host/main.cpp`. "Planned"
ops are on the PR-body follow-up checklist.

### Sandbox → main (events, `requestId: null`)

| `event` | Payload |
|---|---|
| `ready` | `{ pluginName, manufacturer, numParams, hasEditor, latencySamples }` (first message after pipe connect) |
| `parameterChanged` | `{ index, value }` — plugin moved its own knobs (automation, GUI) |
| `editorClosed` | `{ reason }` — user closed window via X, or plugin self-closed |
| `log` | `{ level, message }` — surface plugin stderr / JUCE asserts |
| `error` | `{ code, message }` — non-fatal recoverable error |
| `goodbye` | `{}` — last message before clean exit |

A broken pipe with no `goodbye` means the sandbox crashed.

## 4. Audio channel — shared memory + events

Audio is too latency-sensitive for JSON-on-pipes. One block at 48 k / 256 samples is
5.33 ms; we want round-trip overhead well under 1 ms.

Layout in `audio-shm` (single mapping):

```text
offset  size                                 contents
0       sizeof(Header)                       Header (atomics, indices)
H       maxBlocks × maxBlock × maxCh × 4 B   Ring A (host → sandbox, input audio)
H+R     maxBlocks × maxBlock × maxCh × 4 B   Ring B (sandbox → host, output audio)
H+2R    sizeof(MidiQueue)                    Inline MIDI events for upcoming block
```

```c
struct Header {
    uint32_t version;
    uint32_t maxBlocks;          // typically 4
    uint32_t maxBlockSamples;    // capped at e.g. 1024
    uint32_t maxChannels;        // 2 for stereo
    uint32_t sampleRate;
    // Per-direction indices — needed so input (host→sandbox) and output
    // (sandbox→host) producers/consumers don't share state.
    std::atomic<uint64_t> inWriteIdx;   // host produces ring A
    std::atomic<uint64_t> inReadIdx;    // sandbox consumes ring A
    std::atomic<uint64_t> outWriteIdx;  // sandbox produces ring B
    std::atomic<uint64_t> outReadIdx;   // host consumes ring B
    // diagnostic
    std::atomic<uint64_t> xruns;
    std::atomic<uint64_t> dropouts;
};
```

> **Impl status:** the v1 Windows code in `src/audio/Sandbox/AudioChannel.cpp`
> currently uses a single `writeIdx/readIdx` pair shared across both rings. It
> works for the strictly-serial `processBlock` round-trip (host writes input,
> waits, sandbox writes output, host reads), but is *not* safe for concurrent
> full-duplex use. Splitting into the per-direction pairs above is part of the
> follow-up "MIDI through audio shm" PR alongside sample-accurate automation.

Float32, planar (channel0 then channel1 — matches JUCE's `AudioBuffer<float>`).

### Per-block protocol

Host audio thread:
```text
1. Wait until (inWriteIdx - inReadIdx) < maxBlocks  (drop block + bump xruns if not)
2. Copy input PCM + any MIDI to Ring A[inWriteIdx % maxBlocks]
3. ++inWriteIdx (release)
4. SetEvent(audio-event-in)
5. WaitForSingleObject(audio-event-out, timeout = blockSize / sampleRate * 2)
6. Copy Ring B[outReadIdx % maxBlocks] into output buffer
7. ++outReadIdx (release)
```

Sandbox audio thread (or sandbox main thread's audio callback):
```text
1. WaitForSingleObject(audio-event-in, INFINITE)
2. Read input from Ring A[inReadIdx % maxBlocks]
3. processBlock(in, out) on the plugin
4. Write output to Ring B[outWriteIdx % maxBlocks]
5. ++inReadIdx (release); ++outWriteIdx (release); SetEvent(audio-event-out)
```

Both events are auto-reset. Worst-case added latency vs in-process: one block period
(~5 ms at 48k/256) due to the producer-consumer hop. Acceptable for guitar processing,
not great for live monitoring — same trade-off any sandboxed host has.

## 5. Plugin selection (sandbox vs in-process)

Slopsmith maintains a list of plugin signatures that need the sandbox:

```jsonc
// %APPDATA%/Slopsmith/sandbox-list.json
{
    "needsSandbox": [
        { "match": "manufacturer", "value": "Native Instruments" },
        { "match": "vst3Uid",      "value": "4E545356-24696752-..." },
        { "match": "linksDll",     "value": "Qt5Core.dll" }
    ]
}
```

Resolution order on `loadVST`:
1. If plugin matches an entry → spawn sandbox.
2. Else → in-process (today's path).
3. If in-process load aborts the addon (`SIGABRT`, `STATUS_STACK_BUFFER_OVERRUN`, …),
   the watchdog promotes the plugin's UID into the list automatically, with a
   `learned-from-crash: true` flag, so the next load is sandboxed.

`linksDll` matching needs a quick prescan: open the vst3 file, walk its PE import
table. Cached in `%LOCALAPPDATA%\Slopsmith\plugin-deps.json`.

## 6. Window reparenting into Electron

1. Sandbox creates its editor in its own top-level window (the PoC's `EditorWindow`)
   — but with `WS_POPUP` style instead of an overlapped frame so it has no border.
2. Sandbox sends `editorOpened { hwnd, w, h }` to main.
3. Renderer asks Electron for its `BrowserWindow.getNativeWindowHandle()`. From the
   renderer, a placeholder `<div>` in the plugin chain's UI has a known position and
   size; the main process reads its bounds via IPC from the renderer.
4. Main process calls
   - `SetWindowLongPtrW(pluginHwnd, GWL_STYLE, (style | WS_CHILD) & ~WS_POPUP)`
   - `SetParent(pluginHwnd, electronHwnd)`
   - `SetWindowPos(pluginHwnd, NULL, placeholderX, placeholderY, w, h, SWP_NOZORDER | SWP_FRAMECHANGED)`
5. On placeholder resize/move (renderer → main IPC), main `SetWindowPos`'s and also
   sends `resizeEditor { w, h }` to the sandbox so the plugin re-lays out.
6. On `closeEditor`, main `SetParent(pluginHwnd, NULL)` first (un-embeds so the
   sandbox can DestroyWindow cleanly), then sends `closeEditor`.

Edge cases:
- DPI: the sandbox enables per-monitor DPI awareness; sandbox and Electron must agree.
  Use `SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)`.
- Floating mode (user preference): skip steps 3–4; just send the HWND back informatively.
- Focus: WM_ACTIVATE inside the embedded child can confuse Electron's accelerator
  routing — likely needs a focus-shim subclassed window between Electron and the plugin.

## 7. Crash + restart

States that need to survive a sandbox crash:
- The plugin's parameter values (Slopsmith caches these as the user changes them, so
  free)
- The plugin's opaque state blob (Slopsmith calls `getState` after every "stable"
  change — patch load, preset switch — and caches it)
- The signal-chain position (already in `SignalChain`, not in the sandbox)

Restart flow:
1. Main detects broken pipe → marks slot as crashed; the SignalChain inserts a silent
   passthrough for the slot so audio keeps flowing.
2. UI shows "plugin crashed — retry" on the slot.
3. On retry (auto after 1 s for the first crash; manual for subsequent), spawn a
   fresh `slopsmith-vst-host.exe`.
4. After `ready`, replay: `prepare` → `setState` (last cached blob) → editor reopen
   if it was open.

Crash loop detection: if same plugin crashes 3× within 60 s, stop auto-retrying and
require a manual restart from the UI.

## 8. Build / repo layout

```text
slopsmith-desktop/
├── src/
│   ├── audio/
│   │   ├── NodeAddon.cpp                       (existing — selects sandbox vs in-process at LoadVST)
│   │   ├── VSTHost.cpp                         (existing — used both in-process and inside the sandbox)
│   │   └── Sandbox/
│   │       ├── Protocol.{h,cpp}                (wire protocol — ops, events, encoding)
│   │       ├── ControlChannel.{h,cpp}          (named-pipe request/response + sandbox-event dispatch)
│   │       ├── AudioChannel.{h,cpp}            (shared-memory ring + Win32 events for the audio path)
│   │       ├── SubprocessHandle.{h,cpp}        (sandbox process lifecycle: CreateProcessW, watcher, shutdown)
│   │       ├── SandboxedProcessor.{h,cpp}      (juce::AudioProcessor that forwards into the sandbox)
│   │       ├── SandboxFactory_win.cpp          (Windows: shouldSandbox() + tryLoadSandboxed())
│   │       └── SandboxFactory_stub.cpp         (non-Windows fallback — always returns nullptr)
│   └── vst-host/                               (sandbox subprocess)
│       ├── main.cpp                            (WinMain + JUCE main-thread message pump)
│       └── CMakeLists.txt                      (target: slopsmith-vst-host.exe)
└── CMakeLists.txt                              (top-level — adds the addon + host targets)
```

`slopsmith-vst-host.exe` ships in the Electron app's `resources/` and is launched from
`SandboxedProcessor::initialise()` via `SandboxFactory_win::resolveSandboxExe()`.

## 9. Out of scope for v1

- Cross-platform sandbox (macOS NSView/XPC, Linux X11-embed). v1 is Windows-only.
- AU/LV2 sandboxing. Most LV2 plugins are well-behaved in-process; reconsider if a
  specific plugin proves otherwise.
- Sandbox pooling (multiple plugins per process). Worth it for memory if a user
  loads 10+ NI plugins; not v1.
- Sample-accurate parameter automation across the IPC boundary. v1 sends parameter
  changes through the control channel with whatever latency that gives (~ms).
  v2 can co-opt the audio shm to embed parameter events per-block.
- Editor-side input redirection (keyboard for VST3 `IPlugViewContentScaleSupport`).
  Probably mostly just works through the reparented HWND.

## 10. Estimate

Wall clock for a single engineer, assuming the PoC's foundations:

| Piece | Effort |
|---|---|
| `slopsmith-vst-host.exe` skeleton (extend the PoC) | 1–2 d |
| Control channel (pipe + JSON + 12 message types) | 2–3 d |
| Audio channel (shm + events + ring) | 2–3 d |
| `SandboxedProcessor` glue inside the addon | 2 d |
| Detection list + denylist promotion | 1 d |
| Editor reparenting into Electron | 2–4 d (focus + DPI is fiddly) |
| Crash detection + restart + state cache | 2 d |
| QA pass on the top-10 NI plugins + iterating on weird behaviours | 3–5 d |
| **Total** | **~15–22 working days** |

Roughly 3–4 calendar weeks, in line with the diag report's original estimate, with
none of it spent fighting Qt.
