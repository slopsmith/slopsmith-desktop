# Slopsmith plugin-sandbox — IPC + lifecycle design

Date: 2026-05-13
Companion to: `DIAG-UPDATE.md`
Status: design sketch (not yet implemented)

## 1. Topology

```text
┌────────────────────────────────────┐         ┌──────────────────────────────┐
│ Slopsmith Desktop (Electron main)  │         │  slopsmith-vst-host.exe      │
│                                    │         │  (one per sandboxed plugin)  │
│  Node main process                 │         │                              │
│  └── slopsmith_audio.node          │         │  WinMain → main thread       │
│      └── SignalChain               │         │  └── JUCE MessageManager     │
│          └── SandboxedSlot ◀────┐  │         │      └── one AudioPlugin     │
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
    --control-pipe "\\.\pipe\slopsmith-vst-<uuid>-ctl"
    --audio-shm    "Local\slopsmith-vst-<uuid>-audio"
    --audio-event-in  "Local\slopsmith-vst-<uuid>-evt-in"
    --audio-event-out "Local\slopsmith-vst-<uuid>-evt-out"
    --sample-rate 48000 --max-block 1024 --channels 2
```

Spawned via `CreateProcess`. The main process creates the pipe + shm + events first,
then spawns; the sandbox connects on startup. Watchdog: if no `ready` event arrives
within 5 s, kill and report failure.

`<uuid>` is a v4 UUID generated per spawn so multiple sandboxes coexist cleanly.

## 3. Control channel — named pipe

Transport: `PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE`, bidirectional, async I/O.

Framing per message: `[u32 length-LE] [json body]`. JSON is small and human-readable
for logging; the audio fast path is *not* on this channel.

Every request from main→sandbox carries a `requestId`. The sandbox echoes it on the
matching reply. Events the sandbox originates (parameter automation, log lines)
carry `requestId: null`.

### Main → sandbox

| `op` | Payload | Reply |
|---|---|---|
| `prepare` | `{ sampleRate, blockSize }` | `{ ok, latencySamples, numInputs, numOutputs }` |
| `setBlockSize` | `{ blockSize }` | `{ ok }` |
| `setParameter` | `{ index, value }` | `{ ok }` (omit reply if `fireAndForget: true`) |
| `getState` | `{}` | `{ stateBase64 }` |
| `setState` | `{ stateBase64 }` | `{ ok }` |
| `midiEvent` | `{ frame, bytes: [..] }` | `{ ok }` (fire-and-forget by default) |
| `openEditor` | `{}` | `{ hwnd: "0x...", w, h }` |
| `resizeEditor` | `{ w, h }` | `{ ok }` |
| `closeEditor` | `{}` | `{ ok }` |
| `listParameters` | `{}` | `{ params: [{index,name,defaultValue,...}] }` |
| `shutdown` | `{}` | `{ ok }` then sandbox exits 0 |

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
    std::atomic<uint64_t> writeIdx;   // host advances
    std::atomic<uint64_t> readIdx;    // sandbox advances
    // diagnostic
    std::atomic<uint64_t> xruns;
    std::atomic<uint64_t> dropouts;
};
```

Float32, planar (channel0 then channel1 — matches JUCE's `AudioBuffer<float>`).

### Per-block protocol

Host audio thread:
```text
1. Wait until (writeIdx - readIdx) < maxBlocks  (drop block + bump xruns if not)
2. Copy input PCM + any MIDI to Ring A[writeIdx % maxBlocks]
3. ++writeIdx (release)
4. SetEvent(audio-event-in)
5. WaitForSingleObject(audio-event-out, timeout = blockSize / sampleRate * 2)
6. Copy Ring B[readIdx % maxBlocks] into output buffer
7. ++readIdx (release)
```

Sandbox audio thread (or sandbox main thread's audio callback):
```text
1. WaitForSingleObject(audio-event-in, INFINITE)
2. Read input from Ring A[(readIdx) % maxBlocks]
3. processBlock(in, out) on the plugin
4. Write output to Ring B[(writeIdx) % maxBlocks]
5. ++writeIdx (release); SetEvent(audio-event-out)
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
table. Cached in `~/.cache/slopsmith/plugin-deps.json`.

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
│   │   ├── NodeAddon.cpp           (existing — adds SandboxedSlot path)
│   │   ├── SandboxedSlot.{h,cpp}   (NEW — wraps a sandbox process from the host)
│   │   ├── SandboxControl.{h,cpp}  (NEW — pipe protocol, request/response)
│   │   ├── SandboxAudio.{h,cpp}    (NEW — shm ring + events)
│   │   └── VSTHost.cpp             (existing — used both in-process and inside sandbox)
│   └── vst-host/                   (NEW — sandbox subprocess)
│       ├── main.cpp                (lifted/cleaned-up PoC main.cpp)
│       ├── HostProtocol.cpp        (mirror of SandboxControl on the sandbox side)
│       └── HostAudio.cpp           (mirror of SandboxAudio)
└── CMakeLists.txt                  (adds slopsmith-vst-host.exe target)
```

`slopsmith-vst-host.exe` ships in the Electron app's `resources/bin/` and is launched
from `SandboxedSlot::spawn()` via `process.execPath` + relative lookup.

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
| `SandboxedSlot` glue inside the addon | 2 d |
| Detection list + denylist promotion | 1 d |
| Editor reparenting into Electron | 2–4 d (focus + DPI is fiddly) |
| Crash detection + restart + state cache | 2 d |
| QA pass on the top-10 NI plugins + iterating on weird behaviours | 3–5 d |
| **Total** | **~15–22 working days** |

Roughly 3–4 calendar weeks, in line with the diag report's original estimate, with
none of it spent fighting Qt.
